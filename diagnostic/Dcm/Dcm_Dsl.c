/* -------------------------------- Arctic Core ------------------------------
 * Arctic Core - the open source AUTOSAR platform http://arccore.com
 *
 * Copyright (C) 2009  ArcCore AB <contact@arccore.com>
 *
 * This source code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by the
 * Free Software Foundation; See <http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 * -------------------------------- Arctic Core ------------------------------*/

#include <string.h>
#include "Mcu.h"
#include "Dcm.h"
#include "Dcm_Internal.h"
#include "Det.h"
#include "MemMap.h"
#include "ComM_Dcm.h"
#include "PduR_Dcm.h"
#include "ComStack_Types.h"

#define USE_DEBUG_PRINTF
#include "debug.h"

#define DECREMENT(timer) { if (timer > 0) timer--; }
#define DCM_CONVERT_MS_TO_MAIN_CYCLES(x)  ((x)/DCM_MAIN_FUNCTION_PERIOD_TIME_MS)


/*
 * Type definitions.
 */
typedef struct {
	const Dcm_DslProtocolRxType *protocolRx;
	const Dcm_DslMainConnectionType *mainConnection;
	const Dcm_DslConnectionType *connection;
	const Dcm_DslProtocolRowType *protocolRow;
} DcmDsl_ProtocolConfigurationType;

#define MAX_PARALLEL_PROTOCOLS_ALLOWED		1

typedef struct {
	boolean initRun;
	const Dcm_DslProtocolRowType *preemptedProtocol; // Points to the currently active protocol.
	const Dcm_DslProtocolRowType *activeProtocol; // Points to the currently active protocol.
	Dcm_DslRunTimeProtocolParametersType
			protocolList[MAX_PARALLEL_PROTOCOLS_ALLOWED];
} DcmDsl_RunTimeDataType;

DcmDsl_RunTimeDataType DcmDslRunTimeData = { .initRun = FALSE,
		.preemptedProtocol = NULL, .activeProtocol = NULL };

// ################# DUMMIES START #################

/*
 * Local types
 */

// Global service table, set by DSL used by DSD
Dcm_DsdServiceTableType *DslCurrentServiceTable = NULL;

void ComM_DCM_ActivateDiagnostic() {
	;
}

void ComM_DCM_InactivateDiagnostic() {
	;
}

// ################# HELPER FUNCTIONS START #################

//
// This function reset/stars the session (S3) timer. See requirement
// @DCM141 when that action should be taken.
//
void startS3SessionTimer(Dcm_DslRunTimeProtocolParametersType *runtime,
		const Dcm_DslProtocolRowType *protocolRow) {
	const Dcm_DslProtocolTimingRowType *timeParams;
	timeParams = protocolRow->DslProtocolTimeLimit;
	runtime->S3ServerTimeoutCount = DCM_CONVERT_MS_TO_MAIN_CYCLES(
			timeParams->TimStrS3Server);
}

// - - - - - - - - - - -

//
// This function reset/stars the session (S3) timer. See requirement
// @DCM141 when that action should be taken.
//
void stopS3SessionTimer(Dcm_DslRunTimeProtocolParametersType *runtime) {
	runtime->S3ServerTimeoutCount = 0;
}

// - - - - - - - - - - -

#if 1
//
//	This function implements the requirement @DCM139 when
// 	transition from one session to another.
// 	qqq, strange observation: If S3 timeout we will not change security mode to
// 	locked and that is how I interper the requirement.
//
void changeDiagnosticSession(Dcm_DslRunTimeProtocolParametersType *runtime,
		Dcm_SesCtrlType newSession) {
	switch (runtime->sessionControl) {
	case DCM_DEFAULT_SESSION: // "default".
		break;

	case DCM_PROGRAMMING_SESSION:
	case DCM_EXTENDED_DIAGNOSTIC_SESSION:
	case DCM_SAFTEY_SYSTEM_DIAGNOSTIC_SESSION:
	case DCM_ALL_SESSION_LEVEL:
		runtime->securityLevel = DCM_SEC_LEV_LOCKED; // "0x00".
		break;

	default:
		// TODO: Log this error.
		DEBUG(DEBUG_MEDIUM, "Old session invalid")
		break;
	}

	switch (newSession) {
	case DCM_DEFAULT_SESSION: // "default".
	case DCM_PROGRAMMING_SESSION:
	case DCM_EXTENDED_DIAGNOSTIC_SESSION:
	case DCM_SAFTEY_SYSTEM_DIAGNOSTIC_SESSION:
	case DCM_ALL_SESSION_LEVEL:
		runtime->sessionControl = newSession;
		break;

	default:
		// TODO: Log this error.
		DEBUG(DEBUG_MEDIUM, "New session invalid")
		break;
	}
}
#else
//
//	This function implements the requirement @DCM139 when
// 	transition from one session to another.
// 	qqq, strange observation: If S3 timeout we will not change security mode to
// 	locked and that is how I interper the requirement.
//
void changeDiagnosticSession( Dcm_DslRunTimeProtocolParametersType *runtime,
		Dcm_SesCtrlType newSession) {
	switch (runtime->sessionControl) {
		case DCM_DEFAULT_SESSION: // "default".
		if ( newSession != DCM_DEFAULT_SESSION ) { // "default"
			runtime->securityLevel = DCM_SEC_LEV_LOCKED; // "0x00".
			runtime->sessionControl = newSession;
			runtime->protocolStarted = FALSE;
			DcmDslRunTimeData.activeProtocol = NULL;
		}
		break;
		case DCM_PROGRAMMING_SESSION:
		case DCM_EXTENDED_DIAGNOSTIC_SESSION:
		case DCM_SAFTEY_SYSTEM_DIAGNOSTIC_SESSION:
		case DCM_ALL_SESSION_LEVEL:
		runtime->securityLevel = DCM_SEC_LEV_LOCKED; // "0x00".
		runtime->sessionControl = newSession;
		break;
		default:
		// qqq: Log this error.
		break;
	}
}
#endif

// - - - - - - - - - - -

void DslResetSessionTimeoutTimer() {
	const Dcm_DslProtocolRowType *activeProtocol = NULL;
	Dcm_DslRunTimeProtocolParametersType *runtime = NULL;
	activeProtocol = DcmDslRunTimeData.activeProtocol;
	runtime = activeProtocol->DslRunTimeProtocolParameters;
	startS3SessionTimer(runtime, activeProtocol); // @DCM141
}

// - - - - - - - - - - -

void DslGetCurrentServiceTable(
		const Dcm_DsdServiceTableType **currentServiceTable) {
	const Dcm_DslProtocolRowType *activeProtocol = NULL;
	activeProtocol = DcmDslRunTimeData.activeProtocol;
	if (activeProtocol != NULL) {
		*currentServiceTable = activeProtocol->DslProtocolSIDTable;
	}
}

// - - - - - - - - - - -

Std_ReturnType DslGetActiveProtocol(Dcm_ProtocolType *protocolId) {
	Std_ReturnType ret = E_NOT_OK;
	const Dcm_DslProtocolRowType *activeProtocol = NULL;
	activeProtocol = DcmDslRunTimeData.activeProtocol;
	if (activeProtocol != NULL) {
		*protocolId = activeProtocol->DslProtocolID;
		ret = E_OK;
	}
	return ret;
}

// - - - - - - - - - - -

void DslSetSecurityLevel(Dcm_SecLevelType secLevel) {
	const Dcm_DslProtocolRowType *activeProtocol = NULL;
	Dcm_DslRunTimeProtocolParametersType *runtime = NULL;
	activeProtocol = DcmDslRunTimeData.activeProtocol;
	runtime = activeProtocol->DslRunTimeProtocolParameters;
	runtime->securityLevel = secLevel;
}

// - - - - - - - - - - -

Std_ReturnType DslGetSecurityLevel(Dcm_SecLevelType *secLevel) {
	Std_ReturnType ret = E_NOT_OK;
	const Dcm_DslProtocolRowType *activeProtocol = NULL;
	Dcm_DslRunTimeProtocolParametersType *runtime = NULL;
	activeProtocol = DcmDslRunTimeData.activeProtocol;
	if (activeProtocol != NULL) {
		runtime = activeProtocol->DslRunTimeProtocolParameters;
		*secLevel = runtime->securityLevel;
		ret = E_OK;
	}
	return ret;
}

// - - - - - - - - - - -

void DslSetSesCtrlType(Dcm_SesCtrlType sesCtrl) {
	const Dcm_DslProtocolRowType *activeProtocol = NULL;
	Dcm_DslRunTimeProtocolParametersType *runtime = NULL;
	activeProtocol = DcmDslRunTimeData.activeProtocol;
	if (activeProtocol != NULL) {
		runtime = activeProtocol->DslRunTimeProtocolParameters;
		if (runtime->sessionControl != sesCtrl) {
			changeDiagnosticSession(runtime, sesCtrl);
			DslResetSessionTimeoutTimer();
		}
	}
}

// - - - - - - - - - - -

Std_ReturnType DslGetSesCtrlType(Dcm_SesCtrlType *sesCtrlType) {
	Std_ReturnType ret = E_NOT_OK;
	const Dcm_DslProtocolRowType *activeProtocol = NULL;
	Dcm_DslRunTimeProtocolParametersType *runtime = NULL;
	activeProtocol = DcmDslRunTimeData.activeProtocol;
	if (activeProtocol != NULL) {
		runtime = activeProtocol->DslRunTimeProtocolParameters;
		*sesCtrlType = runtime->sessionControl;
		ret = E_OK;
	}
	return ret;
}

// - - - - - - - - - - -

boolean findRxPduIdParentConfigurationLeafs(PduIdType dcmRxPduId,
		const Dcm_DslProtocolRxType **protocolRx,
		const Dcm_DslMainConnectionType **mainConnection,
		const Dcm_DslConnectionType **connection,
		const Dcm_DslProtocolRowType **protocolRow,
		Dcm_DslRunTimeProtocolParametersType **runtime) {

	boolean ret = FALSE;
	if (dcmRxPduId < DCM_DSL_BUFFER_LIST_LENGTH) {
		*protocolRx
				= &DCM_Config.Dsl->DslProtocol->DslProtocolRxGlobalList[dcmRxPduId];
		*mainConnection = (*protocolRx)->DslMainConnectionParent;
		*connection = (*mainConnection)->DslConnectionParent;
		*protocolRow = (*connection)->DslProtocolRow;
		*runtime = (*protocolRow)->DslRunTimeProtocolParameters;
		ret = TRUE;
	}
	return ret;
}

// - - - - - - - - - - -

boolean findTxPduIdParentConfigurationLeafs(PduIdType dcmTxPduId,
		const Dcm_DslProtocolTxType **protocolTx,
		const Dcm_DslMainConnectionType **mainConnection,
		const Dcm_DslConnectionType **connection,
		const Dcm_DslProtocolRowType **protocolRow,
		Dcm_DslRunTimeProtocolParametersType **runtime) {

	boolean ret = FALSE;
	if (dcmTxPduId < DCM_DSL_BUFFER_LIST_LENGTH) {
		*protocolTx
				= &DCM_Config.Dsl->DslProtocol->DslProtocolTxGlobalList[dcmTxPduId];
		*mainConnection = (*protocolTx)->DslMainConnectionParent;
		*connection = (*mainConnection)->DslConnectionParent;
		*protocolRow = (*connection)->DslProtocolRow;
		*runtime = (*protocolRow)->DslRunTimeProtocolParameters;
		ret = TRUE;
	}
	return ret;
}

// - - - - - - - - - - -

void releaseExternalRxTxBuffers(const Dcm_DslProtocolRowType *protocolRow,
		Dcm_DslRunTimeProtocolParametersType *runtime) {

	protocolRow->DslProtocolTxBufferID->externalBufferRuntimeData->status
			= BUFFER_AVAILABLE;
	protocolRow->DslProtocolRxBufferID->externalBufferRuntimeData->status
			= BUFFER_AVAILABLE;
	runtime->externalTxBufferStatus = NOT_IN_USE; // We are waiting for DSD to return the buffer. qqq.
	runtime->externalRxBufferStatus = NOT_IN_USE; // We are waiting for DSD to return the buffer. qqq.
}

// - - - - - - - - - - -


void releaseExternalRxTxBuffersHelper(PduIdType rxPduIdRef) {
	const Dcm_DslProtocolRxType *protocolRx = NULL;
	const Dcm_DslMainConnectionType *mainConnection = NULL;
	const Dcm_DslConnectionType *connection = NULL;
	const Dcm_DslProtocolRowType *protocolRow = NULL;
	Dcm_DslRunTimeProtocolParametersType *runtime = NULL;

	if (findRxPduIdParentConfigurationLeafs(rxPduIdRef, &protocolRx, &mainConnection,
			&connection, &protocolRow, &runtime)) {
		releaseExternalRxTxBuffers(protocolRow, runtime);
	}
}

/*
 *  This function is called from the DSD module to the DSL when
 *  a response to a diagnostic request has been copied into the
 *  given TX-buffer and is ready for transmission.
 */
void DslDsdProcessingDone(PduIdType rxPduIdRef,
		DsdProcessingDoneResultType responseResult) {
	const Dcm_DslProtocolRxType *protocolRx = NULL;
	const Dcm_DslMainConnectionType *mainConnection = NULL;
	const Dcm_DslConnectionType *connection = NULL;
	const Dcm_DslProtocolRowType *protocolRow = NULL;
	Dcm_DslRunTimeProtocolParametersType *runtime = NULL;

	DEBUG( DEBUG_MEDIUM, "DslDsdProcessingDone rxPduIdRef=%d\n", rxPduIdRef);

	if (findRxPduIdParentConfigurationLeafs(rxPduIdRef, &protocolRx, &mainConnection,
			&connection, &protocolRow, &runtime)) {
		imask_t state = McuE_EnterCriticalSection();
		switch (responseResult) {
		case DSD_TX_RESPONSE_READY:
			runtime->externalTxBufferStatus = DSD_PENDING_RESPONSE_SIGNALED;
			break;
		case DSD_TX_RESPONSE_SUPPRESSED:
			releaseExternalRxTxBuffersHelper(rxPduIdRef);
			break;
		default:
			DEBUG( DEBUG_MEDIUM, "Unknown response result from DslDsdProcessingDone!\n");
			break;
		}
		McuE_ExitCriticalSection(state);
	}
}

/*
 *	This function preparing transmission of response
 *	pending message to tester.
 */
void sendResponse(const Dcm_DslProtocolRowType *protocol,
		Dcm_NegativeResponseCodeType responseCode) {
	Dcm_DslRunTimeProtocolParametersType *runtime = NULL;
	const uint32 txPduId = protocol->DslConnection->DslMainConnection->DslProtocolTx->DcmDslProtocolTxPduId;
	runtime = protocol->DslRunTimeProtocolParameters;
	imask_t state = McuE_EnterCriticalSection();
	if (runtime->localTxBuffer.status == NOT_IN_USE) {
		runtime->localTxBuffer.status = PROVIDED_TO_DSD; // For readability (yes, it will later be overwritten).
		runtime->localTxBuffer.buffer[0] = SID_NEGATIVE_RESPONSE;
		runtime->localTxBuffer.buffer[1]
				= protocol->DslProtocolRxBufferID->pduInfo.SduDataPtr[2];
		runtime->localTxBuffer.buffer[2] = responseCode; // 0x78.
		runtime->localTxBuffer.PduInfo.SduDataPtr
				= runtime->localTxBuffer.buffer;
		runtime->localTxBuffer.PduInfo.SduLength = 3;
		runtime->localTxBuffer.status = DCM_TRANSMIT_SIGNALED; // In the DslProvideTxBuffer 'callback' this state signals it is the local buffer we are intressted in sending.
		PduR_DcmTransmit(txPduId, &(runtime->localTxBuffer.PduInfo));
	}
	McuE_ExitCriticalSection(state);
}

// - - - - - - - - - - -

Std_ReturnType StartProtocolHelper(Dcm_ProtocolType protocolId) {
	Std_ReturnType ret = E_NOT_OK;
	uint16 i;

	for (i = 0; !DCM_Config.Dsl->DslCallbackDCMRequestService[i].Arc_EOL; i++) {
		if (DCM_Config.Dsl->DslCallbackDCMRequestService[i].StartProtocol
				!= NULL) {
			ret
					= DCM_Config.Dsl->DslCallbackDCMRequestService[i]. StartProtocol(
							protocolId);
			if (ret != E_OK) { // qqq: eqvivalent to DCM_E_OK?
				break;
			}
		}
	}
	return ret;
}

// - - - - - - - - - - -

boolean isTesterPresentCommand(const PduInfoType *rxPdu) {
	boolean ret = FALSE;
	if ((rxPdu->SduDataPtr[0] == SID_TESTER_PRESENT) && (rxPdu->SduDataPtr[1]
			& SUPPRESS_POS_RESP_BIT)) {
		return TRUE;
	}
	return ret;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
//	Implements 'void Dcm_Init(void)' for DSL.
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void DslInit(void) {
	const Dcm_DslProtocolRowType *listEntry = NULL;
	Dcm_DslRunTimeProtocolParametersType *runtime = NULL;

	listEntry = DCM_Config.Dsl->DslProtocol->DslProtocolRowList;
	while (listEntry->Arc_EOL == FALSE) {
		runtime = listEntry->DslRunTimeProtocolParameters;
		runtime->externalRxBufferStatus = DCM_IDLE;
		runtime->externalTxBufferStatus = DCM_IDLE;
		runtime->localRxBuffer.status = DCM_IDLE;
		runtime->localTxBuffer.status = DCM_IDLE;
		runtime->securityLevel = DCM_SEC_LEV_LOCKED;
		runtime->sessionControl = DCM_DEFAULT_SESSION;
		listEntry->DslProtocolRxBufferID->externalBufferRuntimeData->status
				= BUFFER_AVAILABLE;
		listEntry->DslProtocolRxBufferID->externalBufferRuntimeData->status
				= BUFFER_AVAILABLE;
		listEntry++;
	};
	DcmDslRunTimeData.initRun = TRUE;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
//	Implements 'void Dcm_MainFunction(void)' for DSL.
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void DslMain(void) {
	const Dcm_DslProtocolRowType *protocolRowEntry = NULL;
	const Dcm_DslProtocolTimingRowType *timeParams = NULL;
	Dcm_DslRunTimeProtocolParametersType *runtime = NULL;
	int debug_count = 0;

	protocolRowEntry = DCM_Config.Dsl->DslProtocol->DslProtocolRowList;
	while (protocolRowEntry->Arc_EOL == FALSE) {
		runtime = protocolRowEntry->DslRunTimeProtocolParameters;
		if (runtime != NULL) {
			if (runtime->sessionControl != DCM_DEFAULT_SESSION) { // Timeout if tester present is lost.
				DECREMENT(runtime->S3ServerTimeoutCount);
				if (runtime->S3ServerTimeoutCount == 0) {
					changeDiagnosticSession(runtime, DCM_DEFAULT_SESSION);
				}
			}
			switch (runtime->externalTxBufferStatus) { // #### TX buffer state. ####
			case NOT_IN_USE:
				//DEBUG( DEBUG_MEDIUM, "state NOT_IN_USE!\n");
				break;
			case PROVIDED_TO_DSD: {
				DEBUG( DEBUG_MEDIUM, "debug_count=%d\n", debug_count);
				DECREMENT(runtime->stateTimeoutCount);
				if (runtime->stateTimeoutCount == 0) {
					timeParams = protocolRowEntry->DslProtocolTimeLimit;
					runtime->stateTimeoutCount = DCM_CONVERT_MS_TO_MAIN_CYCLES(
							timeParams->TimStrP2ServerMax); /* Reinitiate timer, see 9.2.2. */
					if (DCM_Config.Dsl->DslDiagResp->DslDiagRespForceRespPendEn
							== TRUE) {
						if (runtime->responsePendingCount != 0) {
							DEBUG( DEBUG_MEDIUM, "No response withing timeout, sending response pending!\n");
							sendResponse(protocolRowEntry, DCM_E_RESPONSEPENDING);
							DECREMENT( runtime->responsePendingCount );
						} else {
							DEBUG( DEBUG_MEDIUM, "Sent all response pending, now sending general reject!\n");
							sendResponse(protocolRowEntry, DCM_E_GENERALREJECT);
							releaseExternalRxTxBuffers(protocolRowEntry,
									runtime);
						}
					} else {
						DEBUG( DEBUG_MEDIUM, "Not configured to send response pending, now sending general reject!\n");
						sendResponse(protocolRowEntry, DCM_E_GENERALREJECT);
						releaseExternalRxTxBuffers(protocolRowEntry, runtime);
					}
				}
				break;
			}
			case DSD_PENDING_RESPONSE_SIGNALED:
				// The DSD has signaled to DSL that the diagnostic response is available in the Tx buffer.
				// Make sure that response pending or general reject have not been issued,
				// if so we can not transmit to PduR because we would not know from where
				// the Tx confirmation resides later.
				DEBUG( DEBUG_MEDIUM, "Current polite index=debug_count=%d\n", debug_count);
				DEBUG( DEBUG_MEDIUM, "state DSD_PENDING_RESPONSE_SIGNALED!\n");
				if (runtime->localTxBuffer.status == NOT_IN_USE) {
					const uint32 txPduId = protocolRowEntry->DslConnection->DslMainConnection->DslProtocolTx->DcmDslProtocolTxPduId;
					DEBUG( DEBUG_MEDIUM, "runtime->externalTxBufferStatus enter state DSD_PENDING_RESPONSE_SIGNALED.\n", txPduId);
					runtime->externalTxBufferStatus = DCM_TRANSMIT_SIGNALED;
					DEBUG( DEBUG_MEDIUM, "Calling PduR_DcmTransmit with txPduId = %d from DslMain\n", txPduId);
					PduR_DcmTransmit(txPduId,
							&runtime->diagnosticResponseFromDsd); /** @req DCM237 **//* Will trigger PduR (CanTP) to call DslProvideTxBuffer(). */
				}
				break;
			case DCM_TRANSMIT_SIGNALED:
				//DEBUG( DEBUG_MEDIUM, "state DSD_PENDING_RESPONSE_SIGNALED!\n");
				break;
			case PROVIDED_TO_PDUR: // The valid data is being transmitted by TP-layer.
				//DEBUG( DEBUG_MEDIUM, "state DSD_PENDING_RESPONSE_SIGNALED!\n");
				break;
			default:
				break;
			}
		}
		protocolRowEntry++;
		debug_count++;
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
//	Implements 'BufReq_ReturnType Dcm_ProvideRxBuffer(PduIdType dcmRxPduId,
//  PduLengthType tpSduLength, PduInfoType **pduInfoPtr)'.
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
//  This function is called called by the PduR typically when CanTp has
//  received a FF or a single frame and needs to obtain a buffer from the
//  receiver so that received data can be forwarded.

BufReq_ReturnType DslProvideRxBufferToPdur(PduIdType dcmRxPduId,
		PduLengthType tpSduLength, const PduInfoType **pduInfoPtr) {
	BufReq_ReturnType ret = BUFREQ_NOT_OK;
	const Dcm_DslProtocolRxType *protocolRx = NULL;
	const Dcm_DslMainConnectionType *mainConnection = NULL;
	const Dcm_DslConnectionType *connection = NULL;
	const Dcm_DslProtocolRowType *protocolRow = NULL;
	Dcm_DslRunTimeProtocolParametersType *runtime = NULL;

	DEBUG( DEBUG_MEDIUM, "DslProvideRxBufferToPdur(dcmRxPduId=%d) called!\n", dcmRxPduId);
	imask_t state = McuE_EnterCriticalSection();
	if (findRxPduIdParentConfigurationLeafs(dcmRxPduId, &protocolRx, &mainConnection,
			&connection, &protocolRow, &runtime)) {
		const Dcm_DslBufferType *externalRxBuffer =
				protocolRow->DslProtocolRxBufferID;
		if (externalRxBuffer->pduInfo.SduLength >= tpSduLength) { // First validate that we have a chance receiving the chunk of data.
			if ((runtime->externalRxBufferStatus == NOT_IN_USE)
					&& (externalRxBuffer->externalBufferRuntimeData->status
							== BUFFER_AVAILABLE)) {
				DEBUG( DEBUG_MEDIUM, "External buffer available!\n");
				// ### EXTERNAL BUFFER IS AVAILABLE; GRAB IT AND REMEBER THAT WE OWN IT! ###
				externalRxBuffer->externalBufferRuntimeData->status
						= BUFFER_BUSY;
				runtime->diagnosticRequestFromTester.SduDataPtr
						= externalRxBuffer->pduInfo.SduDataPtr;
				runtime->diagnosticRequestFromTester.SduLength = tpSduLength;
				//*pduInfoPtr = &(externalRxBuffer->pduInfo);
				*pduInfoPtr = &(runtime->diagnosticRequestFromTester);
				runtime->externalRxBufferStatus = PROVIDED_TO_PDUR;
				ret = BUFREQ_OK;
			} else {
				DEBUG( DEBUG_MEDIUM, "Local buffer available!\n");
				if (runtime->externalRxBufferStatus == PROVIDED_TO_DSD) {
					// ### EXTERNAL BUFFER IS IN USE BY THE DSD, TRY TO USE LOCAL BUFFER! ###
					if (runtime->localRxBuffer.status == NOT_IN_USE) {
						if (tpSduLength < DCM_DSL_LOCAL_BUFFER_LENGTH) {
							runtime->localRxBuffer.status = PROVIDED_TO_PDUR;
							runtime->localRxBuffer.PduInfo.SduDataPtr
									= runtime->localRxBuffer.buffer;
							runtime->localRxBuffer.PduInfo.SduLength
									= tpSduLength;
							*pduInfoPtr = &(runtime->localRxBuffer.PduInfo);
							ret = BUFREQ_OK;
						}
					}
				} else {
					// The buffer is in use by the PduR, we can not help this because then
					// we would have two different Rx-indications with same PduId but we
					// will not know which buffer the indication should free.
					ret = BUFREQ_BUSY;
				}
			}
		} else {
			ret = BUFREQ_OVFL; // Required size is too big.
		}
		if (ret == BUFREQ_OK) {
			stopS3SessionTimer(runtime); /** req: DCM141 **/
		}
	}
	McuE_ExitCriticalSection(state);
	return ret;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
//	Implements 'void Dcm_RxIndication(PduIdType dcmRxPduId, NotifResultType result)'.
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// 	This function is called called by the PduR typically when CanTp has
//	received the diagnostic request, copied it to the provided buffer and need to indicate
//	this to the DCM (DSL) module via propritary API.

void DslRxIndicationFromPduR(PduIdType dcmRxPduId, NotifResultType result) {
	const Dcm_DslProtocolRxType *protocolRx = NULL;
	const Dcm_DslMainConnectionType *mainConnection = NULL;
	const Dcm_DslConnectionType *connection = NULL;
	const Dcm_DslProtocolRowType *protocolRow = NULL;
	const Dcm_DslProtocolTimingRowType *timeParams = NULL;
	Dcm_DslRunTimeProtocolParametersType *runtime = NULL;
	Std_ReturnType higherLayerResp;
	imask_t state;

	// qqq: handle the actual result code.
	if (findRxPduIdParentConfigurationLeafs(dcmRxPduId, &protocolRx, &mainConnection,
			&connection, &protocolRow, &runtime)) {
		timeParams = protocolRow->DslProtocolTimeLimit;
		// We need to find out in what buffer we can find our Rx data (it can
		// be either in the normal RX-buffer or the 'extra' buffer for implementing
		// the Concurrent "Test Present" functionality.
		if (runtime->externalRxBufferStatus == PROVIDED_TO_PDUR) {
			if (isTesterPresentCommand(
					&(protocolRow->DslProtocolRxBufferID->pduInfo))) {
				state = McuE_EnterCriticalSection();
				startS3SessionTimer(runtime, protocolRow); /** @req DCM141 **//** @req DCM112 **//** @req DCM113 **/
				runtime->externalRxBufferStatus = NOT_IN_USE;
				protocolRow->DslProtocolRxBufferID->externalBufferRuntimeData->status = BUFFER_AVAILABLE;
				McuE_ExitCriticalSection(state);
			} else {
				if (runtime->protocolStarted == FALSE) {
					higherLayerResp = StartProtocolHelper(
							protocolRow->DslProtocolID);
					if (higherLayerResp == E_OK) {
						runtime->protocolStarted = TRUE;
						DcmDslRunTimeData.activeProtocol = protocolRow;
					}
				}
				if (runtime->protocolStarted == TRUE) {
					if (runtime->diagnosticActiveComM == FALSE) {
						ComM_DCM_ActivateDiagnostic(); /* @DCM163 */
						runtime->diagnosticActiveComM = TRUE;
					}
					state = McuE_EnterCriticalSection();
					runtime->stateTimeoutCount = DCM_CONVERT_MS_TO_MAIN_CYCLES(
							timeParams->TimStrP2ServerMax); /* See 9.2.2. */
					runtime->externalRxBufferStatus = PROVIDED_TO_DSD;
					runtime->externalTxBufferStatus = PROVIDED_TO_DSD; // Used in main.
					timeParams = protocolRow->DslProtocolTimeLimit;
					runtime->stateTimeoutCount = DCM_CONVERT_MS_TO_MAIN_CYCLES(
							timeParams->TimStrP2ServerMax); /* Reinitiate timer, see 9.2.2. */
					McuE_ExitCriticalSection(state);
					runtime->diagnosticResponseFromDsd.SduDataPtr
							= protocolRow->DslProtocolTxBufferID->pduInfo.SduDataPtr;
					runtime->diagnosticResponseFromDsd.SduLength
							= protocolRow->DslProtocolTxBufferID->pduInfo.SduLength;
					DEBUG( DEBUG_MEDIUM, "DsdDslDataIndication(DcmDslProtocolTxPduId=%d, dcmRxPduId=%d)\n",
							mainConnection->DslProtocolTx->DcmDslProtocolTxPduId, dcmRxPduId);
					DsdDslDataIndication(
							&(runtime->diagnosticRequestFromTester),
							protocolRow->DslProtocolSIDTable,
							protocolRx->DslProtocolAddrType,
							dcmRxPduId,
							&(runtime->diagnosticResponseFromDsd),
							dcmRxPduId);


				}
			}
		} else {
			// It is the local buffer that was provided to the PduR, that buffer
			// is only used for tester present reception in parallel to diagnostic
			// requests.
			state = McuE_EnterCriticalSection();
			if (runtime->localRxBuffer.status == PROVIDED_TO_PDUR) {
				if (isTesterPresentCommand(&(runtime->localRxBuffer.PduInfo))) {
					startS3SessionTimer(runtime, protocolRow); /** @req DCM141 **//** @req DCM112 **//** @req DCM113 **/
				}
				runtime->localRxBuffer.status = NOT_IN_USE;
			}
			McuE_ExitCriticalSection(state);
		}
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
//	Implements 'BufReq_ReturnType Dcm_ProvideTxBuffer(PduIdType dcmTxPduId,
//  PduInfoType **pduInfoPtr, PduLengthType length)'.
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
//  This TX-buffer request is likely triggered by the transport layer (i.e. CanTp)
//  after PduR_DcmTransmit() has been called (via PduR to CanTp) indicating that something
//  is to be sent. The PduR_DcmTransmit() call is done from the DSL main function when
//  it has detected that the pending request has been answered by DSD
//  (or any other module?).

BufReq_ReturnType DslProvideTxBuffer(PduIdType dcmTxPduId,
		const PduInfoType **pduInfoPtr, PduLengthType length) {
	BufReq_ReturnType ret = BUFREQ_NOT_OK;
	const Dcm_DslProtocolTxType *protocolTx = NULL;
	const Dcm_DslMainConnectionType *mainConnection = NULL;
	const Dcm_DslConnectionType *connection = NULL;
	const Dcm_DslProtocolRowType *protocolRow = NULL;
	Dcm_DslRunTimeProtocolParametersType *runtime = NULL;

	DEBUG( DEBUG_MEDIUM, "DslProvideTxBuffer (dcmTxPduId=%d)\n", dcmTxPduId);
	if (findTxPduIdParentConfigurationLeafs(dcmTxPduId, &protocolTx, &mainConnection,
			&connection, &protocolRow, &runtime)) {
		switch (runtime->externalTxBufferStatus) { // ### EXTERNAL TX BUFFER ###
		case DCM_TRANSMIT_SIGNALED: {
			*pduInfoPtr = &(protocolRow->DslProtocolTxBufferID->pduInfo);
			runtime->externalTxBufferStatus = PROVIDED_TO_PDUR;
			ret = BUFREQ_OK;
			break;
		}
		default:
			DEBUG( DEBUG_MEDIUM, "DCM_TRANSMIT_SIGNALED was not signaled in the external buffer\n");
			ret = BUFREQ_NOT_OK;
			break;
		}
		if (ret == BUFREQ_NOT_OK) {
			switch (runtime->localTxBuffer.status) { // ### LOCAL TX BUFFER ###
			case DCM_TRANSMIT_SIGNALED: {
				runtime->localTxBuffer.PduInfo.SduDataPtr
						= runtime->localTxBuffer.buffer;
				runtime->localTxBuffer.PduInfo.SduLength
						= runtime->localTxBuffer.messageLenght;
				*pduInfoPtr = &runtime->localTxBuffer.PduInfo;
				runtime->localTxBuffer.status = PROVIDED_TO_PDUR; // Now the DSL should not touch this Tx-buffer anymore.
				ret = BUFREQ_OK;
				break;
			}
			default:
				DEBUG( DEBUG_MEDIUM, "DCM_TRANSMIT_SIGNALED was not signaled for the local buffer either\n");
				ret = BUFREQ_NOT_OK;
				break;
			}
		}
	}
	return ret;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
//	Implements 'void Dcm_TxConfirmation(PduIdType dcmTxPduId, NotifResultType result))'.
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// 	This function is called by the PduR (which has been trigged by i.e. CanTp)
// 	when a transmission has been successfully finished, have had errors or
// 	is even stopped.

void DslTxConfirmation(PduIdType dcmTxPduId, NotifResultType result) {
	const Dcm_DslProtocolTxType *protocolTx = NULL;
	const Dcm_DslMainConnectionType *mainConnection = NULL;
	const Dcm_DslConnectionType *connection = NULL;
	const Dcm_DslProtocolRowType *protocolRow = NULL;
	Dcm_DslRunTimeProtocolParametersType *runtime = NULL;
	imask_t state;

	DEBUG( DEBUG_MEDIUM, "DslTxConfirmation=%d\n", dcmTxPduId);
	if (findTxPduIdParentConfigurationLeafs(dcmTxPduId, &protocolTx, &mainConnection,
			&connection, &protocolRow, &runtime)) {
		boolean externalBufferReleased = FALSE;

		// Free the buffer and free the Pdu runtime data buffer.
		state = McuE_EnterCriticalSection();
		switch (runtime->externalTxBufferStatus) { // ### EXTERNAL TX BUFFER ###
		case PROVIDED_TO_PDUR: {
			ComM_DCM_InactivateDiagnostic();
			startS3SessionTimer(runtime, protocolRow); // @DCM141
			releaseExternalRxTxBuffers(protocolRow, runtime);
			/*
			 protocolRow->DslProtocolTxBufferID->externalBufferRuntimeData->status
			 = BUFFER_AVAILABLE;
			 protocolRow->DslProtocolRxBufferID->externalBufferRuntimeData->status
			 = BUFFER_AVAILABLE;
			runtime->externalTxBufferStatus = DCM_IDLE;
			runtime->externalRxBufferStatus = DCM_IDLE;
			 */
			DEBUG( DEBUG_MEDIUM, "Released external buffer sucessfully!\n");
			externalBufferReleased = TRUE;

//			DsdDataConfirmation(
//					mainConnection->DslProtocolTx->PduR_DcmDslTxPduId, result); /** @req DCM117 **//** @req DCM235 **/

			DsdDataConfirmation(dcmTxPduId, result); /** @req DCM117 **//** @req DCM235 **/

			break;
		}
		default:
			break;
		}
		if (externalBufferReleased == FALSE) {
			switch (runtime->localTxBuffer.status) { // ### LOCAL TX BUFFER ###
			case PROVIDED_TO_PDUR:
				DEBUG( DEBUG_MEDIUM, "Released local buffer buffer!\n");
				runtime->localTxBuffer.status = DCM_IDLE;
				break;
			default:
				DEBUG( DEBUG_MEDIUM, "WARNING! DslTxConfirmation could not release any buffer!\n");
				break;
			}
		}
		McuE_ExitCriticalSection(state);
	}
}

