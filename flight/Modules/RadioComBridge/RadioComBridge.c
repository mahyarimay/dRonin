/**
 ******************************************************************************
 * @addtogroup Modules Modules
 * @{ 
 * @addtogroup RadioComBridgeModule Com Port to Radio Bridge Module
 * @{ 
 *
 * @file       RadioComBridge.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2012.
 * @author     Tau Labs, http://taulabs.org, Copyright (C) 2013-2016
 * @author     dRonin, http://dronin.org Copyright (C) 2015-2016
 * @brief      Bridges from RFM22b comm channel to another PIOS_COM channel
 *             has the ability to hook and process UAVO packets for the radio
 *             board (e.g. TauLink)
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>
 *
 * Additional note on redistribution: The copyright and license notices above
 * must be maintained in each individual source file that is a derivative work
 * of this source file; otherwise redistribution is prohibited.
 */

// ****************

#include <openpilot.h>
#include <eventdispatcher.h>
#include <objectpersistence.h>
#include <radiocombridgestats.h>
#include "hwtaulink.h"
#include <uavtalk.h>
#include <uavtalk_priv.h>
#if defined(PIOS_INCLUDE_FLASH_EEPROM)
#include <pios_eeprom.h>
#endif

// these objects are parsed locally for relaying to taranis
#include "flightbatterystate.h"
#include "flightstatus.h"
#include "positionactual.h"
#include "velocityactual.h"
#include "baroaltitude.h"
#include "modulesettings.h"

#include "pios_thread.h"
#include "pios_queue.h"

#include <pios_hal.h>

// ****************
// Private constants

#define STACK_SIZE_BYTES  600
#define TASK_PRIORITY     PIOS_THREAD_PRIO_LOW
#define MAX_RETRIES       2
#define MAX_PORT_DELAY    200
#define RETRY_TIMEOUT_MS  150

// ****************
// Private types

typedef struct {
	// The task handles.
	struct pios_thread *telemetryRxTaskHandle;
	struct pios_thread *radioRxTaskHandle;

	// The UAVTalk connection on the com side.
	UAVTalkConnection telemUAVTalkCon;
	UAVTalkConnection radioUAVTalkCon;

	volatile bool have_port;
} RadioComBridgeData;

// ****************
// Private functions

static void telemetryRxTask(void *parameters);
static void radioRxTask(void *parameters);
static int32_t UAVTalkSendHandler(void *ctx, uint8_t * buf, int32_t length);
static int32_t RadioSendHandler(void *ctx, uint8_t * buf, int32_t length);

static void ProcessLocalStream(UAVTalkConnection inConnectionHandle,
				   UAVTalkConnection outConnectionHandle,
				   uint8_t rxbyte);
static void ProcessRadioStream(UAVTalkConnection inConnectionHandle,
			       UAVTalkConnection outConnectionHandle,
			       uint8_t rxbyte);

// ****************
// Private variables

static RadioComBridgeData *data;

/**
 * @brief Start the module
 *
 * @return -1 if initialisation failed, 0 on success
 */
static int32_t RadioComBridgeStart(void)
{
	if (data) {
		// Watchdog must be registered before starting tasks
#ifdef PIOS_INCLUDE_WDG
		PIOS_WDG_RegisterFlag(PIOS_WDG_TELEMETRY);
		PIOS_WDG_RegisterFlag(PIOS_WDG_RADIORX);
#endif

		// Start the primary tasks for receiving/sending UAVTalk
		// packets from the GCS.  The main telemetry subsystem
		// gets the local ports first (and we trust it to
		// configure them for us).
		data->telemetryRxTaskHandle =
			PIOS_Thread_Create(telemetryRxTask, "telemetryRxTask",
					STACK_SIZE_BYTES, NULL, TASK_PRIORITY);
			    
		data->radioRxTaskHandle =
			PIOS_Thread_Create(radioRxTask, "radioRxTask",
					STACK_SIZE_BYTES, NULL, TASK_PRIORITY);

		return 0;
	}

	return -1;
}

/**
 * @brief Initialise the module
 *
 * @return -1 if initialisation failed on success
 */
static int32_t RadioComBridgeInitialize(void)
{
	// allocate and initialize the static data storage only if module is enabled
	data = PIOS_malloc(sizeof(RadioComBridgeData));
	if (!data) {
		return -1;
	}
	// Initialize the UAVObjects that we use
	if (RadioComBridgeStatsInitialize() == -1) {
		return -1;
	}

	// Initialise UAVTalk
	data->telemUAVTalkCon = UAVTalkInitialize(data, &UAVTalkSendHandler,
			NULL, NULL, NULL);
	data->radioUAVTalkCon = UAVTalkInitialize(NULL, &RadioSendHandler,
			NULL, NULL, NULL);
	if (data->telemUAVTalkCon == 0 || data->radioUAVTalkCon == 0) {
		return -1;
	}

	return 0;
}

MODULE_INITCALL(RadioComBridgeInitialize, RadioComBridgeStart);

/**
 * Update telemetry statistics
 */
#if 0
static void updateRadioComBridgeStats()
{
	UAVTalkStats telemetryUAVTalkStats;
	UAVTalkStats radioUAVTalkStats;
	RadioComBridgeStatsData radioComBridgeStats;

	// Get telemetry stats
	UAVTalkGetStats(data->telemUAVTalkCon, &telemetryUAVTalkStats);

	// Get radio stats
	UAVTalkGetStats(data->radioUAVTalkCon, &radioUAVTalkStats);

	// Get stats object data
	RadioComBridgeStatsGet(&radioComBridgeStats);

	// Update stats object
	radioComBridgeStats.TelemetryTxBytes +=
	    telemetryUAVTalkStats.txBytes;
	radioComBridgeStats.TelemetryTxFailures +=
	    telemetryUAVTalkStats.txErrors;

	radioComBridgeStats.TelemetryRxBytes +=
	    telemetryUAVTalkStats.rxBytes;
	radioComBridgeStats.TelemetryRxFailures +=
	    telemetryUAVTalkStats.rxErrors;

	radioComBridgeStats.RadioTxBytes += radioUAVTalkStats.txBytes;
	radioComBridgeStats.RadioTxFailures += radioUAVTalkStats.txErrors;

	radioComBridgeStats.RadioRxBytes += radioUAVTalkStats.rxBytes;
	radioComBridgeStats.RadioRxFailures += radioUAVTalkStats.rxErrors;

	// Update stats object data
	RadioComBridgeStatsSet(&radioComBridgeStats);
}
#endif

/**
 * @brief Radio rx task.  Receive data packets from the radio and pass them on.
 *
 * @param[in] parameters  The task parameters
 */
static void radioRxTask( __attribute__ ((unused))
			void *parameters)
{
	// Task loop
	while (1) {
#ifdef PIOS_INCLUDE_WDG
		PIOS_WDG_UpdateFlag(PIOS_WDG_RADIORX);
#endif
		if (PIOS_COM_RADIOBRIDGE &&
				PIOS_COM_Available(PIOS_COM_RADIOBRIDGE)) {
			uint8_t serial_data[32];
			uint16_t bytes_to_process =
			    PIOS_COM_ReceiveBuffer(PIOS_COM_RADIOBRIDGE,
						   serial_data,
						   sizeof(serial_data),
						   MAX_PORT_DELAY);
			if (bytes_to_process > 0) {
				// Pass the data through the UAVTalk parser.
				for (uint8_t i = 0;
				     i < bytes_to_process; i++) {
					ProcessRadioStream(data->radioUAVTalkCon,
							   data->telemUAVTalkCon,
							   serial_data[i]);
				}
			}

			/* XXX periodically inject ComBridgeStats to downstream */
			/* XXX periodically inject sessionmanaging to downstream */
		} else {
			/* hand port to telemetry */
			data->have_port = false;
			PIOS_Thread_Sleep(5);
			telemetry_set_inhibit(false);
			PIOS_Thread_Sleep(5);
		}
	}
}

/**
 * @brief Receive telemetry from the USB/COM port.
 *
 * @param[in] parameters  The task parameters
 */
static void telemetryRxTask( __attribute__ ((unused))
			    void *parameters)
{
	// Task loop
	while (1) {
		uint32_t inputPort = PIOS_COM_TELEMETRY;
#ifdef PIOS_INCLUDE_WDG
		PIOS_WDG_UpdateFlag(PIOS_WDG_TELEMETRY);
#endif

		if ((!PIOS_COM_Available(PIOS_COM_RADIOBRIDGE)) ||
				(!data->have_port)) {
			PIOS_Thread_Sleep(5);
			continue;
		}
#if defined(PIOS_INCLUDE_USB)
		// Determine output port (USB takes priority over telemetry port)
		if (PIOS_COM_Available(PIOS_COM_TELEM_USB)) {
			inputPort = PIOS_COM_TELEM_USB;
		}
#endif /* PIOS_INCLUDE_USB */
		if (inputPort) {
			uint8_t serial_data[32];
			uint16_t bytes_to_process =
			    PIOS_COM_ReceiveBuffer(inputPort, serial_data,
						   sizeof(serial_data),
						   MAX_PORT_DELAY);

			if (data->have_port && (bytes_to_process > 0)) {
				PIOS_ANNUNC_Toggle(PIOS_LED_RX);
				for (uint8_t i = 0; i < bytes_to_process;
				     i++) {
					ProcessLocalStream(
						data->telemUAVTalkCon,
						data->radioUAVTalkCon,
						serial_data[i]);
				}
			}
		} else {
			PIOS_Thread_Sleep(5);
		}
	}
}

/**
 * @brief Transmit data buffer to the com port.
 *
 * @param[in] buf Data buffer to send
 * @param[in] length Length of buffer
 * @return -1 on failure
 * @return number of bytes transmitted on success
 */
static int32_t UAVTalkSendHandler(void *ctx, uint8_t * buf, int32_t length)
{
	(void) ctx;

	int32_t ret;
	uint32_t outputPort = PIOS_COM_TELEMETRY;

#if defined(PIOS_INCLUDE_USB)
	// Determine output port (USB takes priority over telemetry port)
	if (PIOS_COM_Available(PIOS_COM_TELEM_USB)) {
		outputPort = PIOS_COM_TELEM_USB;
	}
#endif /* PIOS_INCLUDE_USB */

	if (outputPort) {
		ret = PIOS_COM_SendBufferStallTimeout(outputPort, buf, length,
				RETRY_TIMEOUT_MS);
	} else {
		ret = -1;
	}
	return ret;
}

/**
 * Transmit data buffer to the com port.
 *
 * @param[in] buf Data buffer to send
 * @param[in] length Length of buffer
 * @return -1 on failure
 * @return number of bytes transmitted on success
 */
static int32_t RadioSendHandler(void *ctx, uint8_t * buf, int32_t length)
{
	(void) ctx;

	uint32_t outputPort = PIOS_COM_RADIOBRIDGE;

	// Don't send any data unless the radio port is available.
	if (outputPort && PIOS_COM_Available(outputPort)) {
		return PIOS_COM_SendBufferStallTimeout(outputPort, buf, length,
				RETRY_TIMEOUT_MS);
	} else {
		return -1;
	}
}

#define MetaObjectId(x) (x+1)
/**
 * @brief Process a byte of data received on the telemetry stream
 *
 * @param[in] inConnectionHandle  The UAVTalk connection handle on the telemetry port
 * @param[in] outConnectionHandle  The UAVTalk connection handle on the radio port.
 * @param[in] rxbyte  The received byte.
 */
static void ProcessLocalStream(UAVTalkConnection inConnectionHandle,
				   UAVTalkConnection outConnectionHandle,
				   uint8_t rxbyte)
{
	// Keep reading until we receive a completed packet.
	UAVTalkRxState state =
	    UAVTalkProcessInputStreamQuiet(inConnectionHandle, rxbyte);

	if (state == UAVTALK_STATE_COMPLETE) {
		// all packets are transparently relayed to the remote modem
		// Incidentally this means that we fail requests for the
		// radio stats and only telemeter them, but I consider this
		// okee-dokee.
		UAVTalkRelayPacket(inConnectionHandle, outConnectionHandle);
	}
}

/**
 * @brief Process a byte of data received on the radio data stream.
 *
 * @param[in] inConnectionHandle  The UAVTalk connection handle on the radio port.
 * @param[in] outConnectionHandle  The UAVTalk connection handle on the telemetry port.
 * @param[in] rxbyte  The received byte.
 */
static void ProcessRadioStream(UAVTalkConnection inConnectionHandle,
			       UAVTalkConnection outConnectionHandle,
			       uint8_t rxbyte)
{
	// Keep reading until we receive a completed packet.
	UAVTalkRxState state =
	    UAVTalkProcessInputStreamQuiet(inConnectionHandle, rxbyte);

	if (!data->have_port) {
		telemetry_set_inhibit(true);
		data->have_port = true;
	}

	if (state == UAVTALK_STATE_COMPLETE) {
		// We only want to unpack certain objects from the remote modem
		// Similarly we only want to relay certain objects to the telemetry port
		uint32_t objId = UAVTalkGetPacketObjId(inConnectionHandle);
		switch (objId) {
		case HWTAULINK_OBJID:
		case MetaObjectId(HWTAULINK_OBJID):
			// Ignore object...
			// These objects are shadowed by the modem and are not transmitted to the telemetry port
			// - RFM22BSTATUS_OBJID : ground station will receive the OPLM link status instead
			// - HWTAULINK_OBJID : ground station will read and write the OPLM settings instead
			break;
		case FLIGHTBATTERYSTATE_OBJID:
		case FLIGHTSTATUS_OBJID:
		case POSITIONACTUAL_OBJID:
		case VELOCITYACTUAL_OBJID:
		case BAROALTITUDE_OBJID:
			// process / store locally for relaying to taranis
			UAVTalkReceiveObject(inConnectionHandle);
			UAVTalkRelayPacket(inConnectionHandle, outConnectionHandle);
			break;
		default:
			// all other packets are relayed to the telemetry port
			UAVTalkRelayPacket(inConnectionHandle,
					   outConnectionHandle);
			break;
		}
	}
}
