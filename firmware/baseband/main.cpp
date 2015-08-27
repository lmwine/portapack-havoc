/*
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 *
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "ch.h"
#include "test.h"

#include "lpc43xx_cpp.hpp"

#include "portapack_shared_memory.hpp"
#include "portapack_dma.hpp"

#include "gpdma.hpp"

#include "baseband_dma.hpp"

#include "event_m4.hpp"

#include "irq_ipc_m4.hpp"

#include "rssi.hpp"
#include "rssi_dma.hpp"

#include "touch_dma.hpp"

#include "dsp_decimate.hpp"
#include "dsp_demodulate.hpp"
#include "dsp_fir_taps.hpp"
#include "dsp_iir.hpp"
#include "dsp_iir_config.hpp"
#include "dsp_squelch.hpp"

#include "baseband_stats_collector.hpp"
#include "rssi_stats_collector.hpp"

#include "channel_decimator.hpp"
#include "baseband_processor.hpp"
#include "proc_am_audio.hpp"
#include "proc_nfm_audio.hpp"
#include "proc_wfm_audio.hpp"

#include "clock_recovery.hpp"
#include "access_code_correlator.hpp"
#include "packet_builder.hpp"

#include "message_queue.hpp"

#include "utility.hpp"

#include "debug.hpp"

#include "audio.hpp"
#include "audio_dma.hpp"

#include "gcc.hpp"

#include <cstdint>
#include <cstddef>
#include <array>
#include <string>
#include <bitset>

constexpr auto baseband_thread_priority = NORMALPRIO + 20;
constexpr auto rssi_thread_priority = NORMALPRIO + 10;

class FSKProcessor : public BasebandProcessor {
public:
	FSKProcessor(
		MessageHandlerMap& message_handlers
	) : message_handlers(message_handlers)
	{
		message_handlers.register_handler(Message::ID::FSKConfiguration,
			[this](const Message* const p) {
				auto m = reinterpret_cast<const FSKConfigurationMessage*>(p);
				this->configure(m->configuration);
			}
		);
	}

	~FSKProcessor() {
		message_handlers.unregister_handler(Message::ID::FSKConfiguration);
	}

	void configure(const FSKConfiguration new_configuration) {
		demod.configure(76800, 2 * new_configuration.symbol_rate);
		clock_recovery.configure(new_configuration.symbol_rate, 76800);
		access_code_correlator.configure(
			new_configuration.access_code,
			new_configuration.access_code_length,
			new_configuration.access_code_tolerance
		);
		packet_builder.configure(new_configuration.packet_length);
	}

	void execute(buffer_c8_t buffer) override {
		/* 2.4576MHz, 2048 samples */

		auto decimator_out = decimator.execute(buffer);

		/* 153.6kHz, 128 samples */

		const buffer_c16_t work_baseband_buffer {
			(complex16_t*)decimator_out.p,
			decimator_out.count
		};

		/* 153.6kHz complex<int16_t>[128]
		 * -> FIR filter, <?kHz (?fs) pass, gain 1.0
		 * -> 76.8kHz int16_t[64] */
		auto channel = channel_filter.execute(decimator_out, work_baseband_buffer);

		/* 76.8kHz, 64 samples */
		feed_channel_stats(channel);
		feed_channel_spectrum(
			channel,
			decimator_out.sampling_rate * channel_filter_taps.pass_frequency_normalized,
			decimator_out.sampling_rate * channel_filter_taps.stop_frequency_normalized
		);

		const auto symbol_handler_fn = [this](const float value) {
			const uint_fast8_t symbol = (value >= 0.0f) ? 1 : 0;
			const bool access_code_found = this->access_code_correlator.execute(symbol);
			this->consume_symbol(symbol, access_code_found);
		};

		// 76.8k

		const buffer_s16_t work_demod_buffer {
			(int16_t*)decimator_out.p,
			decimator_out.count * sizeof(*decimator_out.p) / sizeof(int16_t)
		};

		auto demodulated = demod.execute(channel, work_demod_buffer);

		i2s::i2s0::tx_mute();

		for(size_t i=0; i<demodulated.count; i++) {
			clock_recovery.execute(demodulated.p[i], symbol_handler_fn);
		}
	}

private:
	ChannelDecimator decimator { ChannelDecimator::DecimationFactor::By16 };
	const fir_taps_real<64>& channel_filter_taps = taps_64_lp_031_070_tfilter;
	dsp::decimate::FIRAndDecimateBy2Complex<64> channel_filter { channel_filter_taps.taps };
	dsp::demodulate::FM demod { 76800, 9600 * 2 };

	ClockRecovery clock_recovery;
	AccessCodeCorrelator access_code_correlator;
	PacketBuilder packet_builder;

	MessageHandlerMap& message_handlers;

	void consume_symbol(
		const uint_fast8_t symbol,
		const bool access_code_found
	) {
		const auto payload_handler_fn = [this](
			const std::bitset<256>& payload,
			const size_t bits_received
		) {
			this->payload_handler(payload, bits_received);
		};

		packet_builder.execute(
			symbol,
			access_code_found,
			payload_handler_fn
		);
	}

	void payload_handler(
		const std::bitset<256>& payload,
		const size_t bits_received
	) {
		FSKPacketMessage message;
		message.packet.payload = payload;
		message.packet.bits_received = bits_received;
		shared_memory.application_queue.push(message);
	}
};

static BasebandProcessor* baseband_processor { nullptr };
static BasebandConfiguration baseband_configuration;

static WORKING_AREA(baseband_thread_wa, 8192);
static __attribute__((noreturn)) msg_t baseband_fn(void *arg) {
	(void)arg;
	chRegSetThreadName("baseband");

	BasebandStatsCollector stats;

	while(true) {
		// TODO: Place correct sampling rate into buffer returned here:
		const auto buffer_tmp = baseband::dma::wait_for_rx_buffer();
		const buffer_c8_t buffer {
			buffer_tmp.p, buffer_tmp.count, baseband_configuration.sampling_rate
		};

		if( baseband_processor ) {
			baseband_processor->execute(buffer);
		}

		stats.process(buffer,
			[](const BasebandStatistics statistics) {
				BasebandStatisticsMessage message;
				message.statistics = statistics;
				shared_memory.application_queue.push(message);
			}
		);
	}
}

static WORKING_AREA(rssi_thread_wa, 128);
static __attribute__((noreturn)) msg_t rssi_fn(void *arg) {
	(void)arg;
	chRegSetThreadName("rssi");

	RSSIStatisticsCollector stats;

	while(true) {
		// TODO: Place correct sampling rate into buffer returned here:
		const auto buffer_tmp = rf::rssi::dma::wait_for_buffer();
		const rf::rssi::buffer_t buffer {
			buffer_tmp.p, buffer_tmp.count, 400000
		};

		stats.process(
			buffer,
			[](const RSSIStatistics statistics) {
				RSSIStatisticsMessage message;
				message.statistics = statistics;
				shared_memory.application_queue.push(message);
			}
		);
	}
}

extern "C" {

void __late_init(void) {
	/*
	 * System initializations.
	 * - HAL initialization, this also initializes the configured device drivers
	 *   and performs the board-specific initializations.
	 * - Kernel initialization, the main() function becomes a thread and the
	 *   RTOS is active.
	 */
	halInit();

	/* After this call, scheduler, systick, heap, etc. are available. */
	/* By doing chSysInit() here, it runs before C++ constructors, which may
	 * require the heap.
	 */
	chSysInit();
}

}

static void init() {
	i2s::i2s0::configure(
		audio::i2s0_config_tx,
		audio::i2s0_config_rx,
		audio::i2s0_config_dma
	);

	audio::dma::init();
	audio::dma::configure();
	audio::dma::enable();

	i2s::i2s0::tx_start();
	i2s::i2s0::rx_start();

	LPC_CREG->DMAMUX = portapack::gpdma_mux;
	gpdma::controller.enable();
	nvicEnableVector(DMA_IRQn, CORTEX_PRIORITY_MASK(LPC_DMA_IRQ_PRIORITY));

	baseband::dma::init();

	rf::rssi::init();
	touch::dma::init();

	chThdCreateStatic(baseband_thread_wa, sizeof(baseband_thread_wa),
		baseband_thread_priority, baseband_fn,
		nullptr
	);

	chThdCreateStatic(rssi_thread_wa, sizeof(rssi_thread_wa),
		rssi_thread_priority, rssi_fn,
		nullptr
	);
}

static void shutdown() {
	// TODO: Is this complete?
	
	nvicDisableVector(DMA_IRQn);

	m0apptxevent_interrupt_disable();
	
	chSysDisable();

	systick_stop();
}

class EventDispatcher {
public:
	MessageHandlerMap& message_handlers() {
		return message_map;
	}

	void run() {
		while(is_running) {
			const auto events = wait();
			dispatch(events);
		}
	}

	void request_stop() {
		is_running = false;
	}

private:
	MessageHandlerMap message_map;

	bool is_running = true;

	eventmask_t wait() {
		return chEvtWaitAny(ALL_EVENTS);
	}

	void dispatch(const eventmask_t events) {
		if( events & EVT_MASK_BASEBAND ) {
			handle_baseband_queue();
		}

		if( events & EVT_MASK_SPECTRUM ) {
			handle_spectrum();
		}
	}

	void handle_baseband_queue() {
		while( !shared_memory.baseband_queue.is_empty() ) {
			std::array<uint8_t, Message::MAX_SIZE> message_buffer;
			const Message* const message = reinterpret_cast<Message*>(message_buffer.data());
			const auto message_size = shared_memory.baseband_queue.pop(message_buffer.data(), message_buffer.size());
			if( message_size ) {
				message_map.send(message);
			}
		}
	}

	void handle_spectrum() {
		if( baseband_processor ) {
			baseband_processor->update_spectrum();
		}
	}
};

static constexpr auto direction = baseband::Direction::Receive;

int main(void) {
	init();

	events_initialize(chThdSelf());
	m0apptxevent_interrupt_enable();

	EventDispatcher event_dispatcher;
	auto& message_handlers = event_dispatcher.message_handlers();

	message_handlers.register_handler(Message::ID::BasebandConfiguration,
		[&message_handlers](const Message* const p) {
			auto message = reinterpret_cast<const BasebandConfigurationMessage*>(p);
			if( message->configuration.mode != baseband_configuration.mode ) {

				// TODO: Timing problem around disabling DMA and nulling and deleting old processor
				auto old_p = baseband_processor;
				baseband_processor = nullptr;
				delete old_p;

				switch(message->configuration.mode) {
				case 0:
					baseband_processor = new NarrowbandAMAudio();
					break;

				case 1:
					baseband_processor = new NarrowbandFMAudio();
					break;

				case 2:
					baseband_processor = new WidebandFMAudio();
					break;

				case 3:
					baseband_processor = new FSKProcessor(message_handlers);
					break;

				default:
					break;
				}

				if( baseband_processor ) {
					if( direction == baseband::Direction::Receive ) {
						rf::rssi::start();
					}
					baseband::dma::enable(direction);
				} else {
					baseband::dma::disable();
					rf::rssi::stop();
				}
			}

			baseband_configuration = message->configuration;
		}
	);

	message_handlers.register_handler(Message::ID::Shutdown,
		[&event_dispatcher](const Message* const) {
			event_dispatcher.request_stop();
		}
	);

	/* TODO: Ensure DMAs are configured to point at first LLI in chain. */

	if( direction == baseband::Direction::Receive ) {
		rf::rssi::dma::allocate(4, 400);
	}

	touch::dma::allocate();
	touch::dma::enable();

	const auto baseband_buffer =
		new std::array<baseband::sample_t, 8192>();
	baseband::dma::configure(
		baseband_buffer->data(),
		direction
	);
	//baseband::dma::allocate(4, 2048);

	event_dispatcher.run();

	shutdown();

	ShutdownMessage shutdown_message;
	shared_memory.application_queue.push(shutdown_message);

	return 0;
}
