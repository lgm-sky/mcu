/*
 ## Cypress CX3 Firmware Example Source (cycx3_uvc.c)
 ## ===========================
 ##
 ##  Copyright Cypress Semiconductor Corporation, 2010-2018,
 ##  All Rights Reserved
 ##  UNPUBLISHED, LICENSED SOFTWARE.
 ##
 ##  CONFIDENTIAL AND PROPRIETARY INFORMATION
 ##  WHICH IS THE PROPERTY OF CYPRESS.
 ##
 ##  Use of this file is governed
 ##  by the license agreement included in the file
 ##
 ##     <install>/license/license.txt
 ##
 ##  where <install> is the Cypress software
 ##  installation root directory path.
 ##
 ## ===========================
 */

/* This application example implements a USB UVC 1.1 compliant video camera on the CX3 using an
 * Omnivision OV5640 image sensor. The example supports the following video formats:
 *      1. Uncompressed 16 bit YUV2 2952x1944 @15 fps over USB SuperSpeed
 *      2. Uncompressed 16 bit YUV2 1920x1080 @30 fps over USB SuperSpeed
 *      3. Uncompressed 16 bit YUV2 1280x720 @60 fps over USB SuperSpeed
 *      4. Uncompressed 16 bit YUV2 640x480 @60 fps over USB Hi-Speed
 *      5. Uncompressed 16 bit YUV2 640x480 @30 fps over USB Hi-Speed
 *      6. Uncompressed 16 bit YUV2 320x240 @5 fps over USB Full Speed
 */

#include "cyfxversion.h"
#include "cyu3system.h"
#include "cyu3os.h"
#include "cyu3dma.h"
#include "cyu3error.h"
#include "cyu3usb.h"
#include "cyu3i2c.h"
#include "cyu3uart.h"
#include "cyu3gpio.h"
#include "cyu3utils.h"
#include "cyu3pib.h"
#include "cyu3socket.h"
#include "sock_regs.h"
#include "cycx3_uvc.h"
#include "cyu3mipicsi.h"
#include "cyu3imagesensor.h"



//add
#include "cyfx3hid.h"



#define Debug(FORMAT,...)\
	do{\
		CyU3PDebugPrint (4, "%s@@%s[%d] " FORMAT "\r\n",__FILE__,__func__,__LINE__,##__VA_ARGS__);\
	}while(0);





static CyU3PThread cx3AppThread;               /* Application thread used for streaming from MIPI interface to USB */
static CyU3PEvent  glCx3Event;                 /* Application Event Group */

#ifdef CX3_ERROR_THREAD_ENABLE
static CyU3PThread cx3MipiErrorThread;         /* Thread used to poll the MIPI interface for Mipi bus errors */
static CyU3PEvent  glMipiErrorEvent;           /* Application Event Group */
#endif

static volatile uint32_t glDmaDone = 0;        /* Tracks number of active data buffers. */
static CyBool_t glMipiActive = CyFalse;        /* Whether MIPI interface is active. Used for Suspend/Resume. */
static CyBool_t glIsClearFeature = CyFalse;    /* Flag to signal when AppStop is called from the ClearFeature or
                                                  SetInterface request. This is used to clear endpoint data toggles. */

/* UVC Header */
static uint8_t glUVCHeader[CX3_APP_HEADER_LENGTH] =
{
    0x0C,                           /* Header Length */
    0x8C,                           /* Bit field header field */
    0x00,0x00,0x00,0x00,            /* Presentation time stamp field */
    0x00,0x00,0x00,0x00,0x00,0x00   /* Source clock reference field */
};

static uint8_t              glCurrentFrameIndex = 1;            /* Current Frame Index */
static CyU3PDmaMultiChannel glChHandleUVCStream;                /* DMA Channel Handle for UVC Stream  */
static CyBool_t             glIsApplnActive = CyFalse;          /* Whether the Mipi->USB application is active or not. */
static CyBool_t             glIsConfigured = CyFalse;           /* Whether Application is in configured state or not */
static CyBool_t             glIsStreamingStarted = CyFalse;     /* Whether streaming has started - Used for MAC OS support*/

/* Video Probe Commit Control */
static uint8_t glCommitCtrl[CX3_APP_MAX_PROBE_SETTING_ALIGNED];

/* Mac OS does not send EP Clear feature when the app is closed. It just stops issuing IN tokens. So buffer commit failures
 * can be counted and if it reaches beyond a limit, streaming can be stopped. Buffer commit failure code can be cleared
 * on DMA Consumer event so that the limit is not reached under streaming conditions.  */


#ifdef RESET_TIMER_ENABLE
static volatile uint32_t glFailFrameCount = 0;                  /* Number of video frames that have failed. */
static CyU3PTimer Cx3ResetTimer;                                /* Timer used to track frame transfer time. */

static void
CyCx3AppProgressTimer (
        uint32_t arg)
{
    /* This frame has taken too long to complete. Notify the thread to abort the frame and restart streaming. */
    CyU3PEventSet(&glCx3Event, CX3_DMA_RESET_EVENT,CYU3P_EVENT_OR);
}
#endif

#ifdef STILL_CAPTURE_ENABLE
static          CyU3PEvent glStillImageEvent;                   /* Still image event group */
static          uint8_t    glStillReq = 0;                      /* Still Trigger Control Array */
static          uint8_t    glStillFrameIndex = 1;               /* Frame Index for Still Capture */
static volatile CyBool_t   glStillFlag = CyFalse;               /* Still Image Event Flag */
static          CyBool_t   glFrameEnd = CyFalse;                /* Still Image Frame End Flag */
static          uint8_t    glStillCommitCtrl[CX3_APP_MAX_PROBE_SETTING_ALIGNED];        /* Still Commit Control Array */

/* Still Probe Control Setting */
uint8_t glStillProbeCtrl[CX3_APP_MAX_STILL_PROBE_SETTING] =
{
    0x01,                            /* Use 1st Video format index */
    0x01,                            /* Use 1st Video frame index */
    0x00,							 /* Compression quality */
    0x00, 0xC6, 0x99, 0x00,          /* Max video frame size in bytes (Highest resolution - 5MP frame size) */
    0x00, 0x80, 0x00, 0x00           /* No. of bytes device can rx in single payload = 16KB */
};
#endif


/* Application critical error handler */
void
CyCx3AppErrorHandler (
        CyU3PReturnStatus_t status        /* API return status */
        )
{
    /* Application failed with the error code status */

    /* Add custom debug or recovery actions here */

    /* Loop indefinitely */
    for (;;)
    {
        /* Thread sleep : 100 ms */
        CyU3PThreadSleep (100);
    }
}


/* UVC header addition function */
static void
CyCx3AppAddHeader (
        uint8_t *buffer_p,      /* Buffer pointer */
        uint8_t frameInd        /* EOF or normal frame indication */
        )
{
	//此函数中不能打印debug信息否则图像不输出
	//CyU3PDebugPrint (4, "\n\r%s:%d", __func__,__LINE__);

    /* Copy header to buffer */
    CyU3PMemCopy (buffer_p, (uint8_t *)glUVCHeader, CX3_APP_HEADER_LENGTH);

    /* Check if last packet of the frame. */
    if (frameInd == CX3_APP_HEADER_EOF)
    {
        /* Modify UVC header to toggle Frame ID */
        glUVCHeader[1] ^= CX3_APP_HEADER_FRAME_ID;

        /* Indicate End of Frame in the buffer */
        buffer_p[1] |=  CX3_APP_HEADER_EOF;
    }
}

CyU3PReturnStatus_t CyFxUsbHidApplnStart(void)
{
	Debug();
	 CyU3PReturnStatus_t status = CY_U3P_SUCCESS;
	return status ;
}
CyU3PReturnStatus_t CyFxUsbHidApplnStop(void)
{
	Debug();
	 CyU3PReturnStatus_t status = CY_U3P_SUCCESS;
	return status ;
}

void CyFxAppErrorHandler(CyU3PReturnStatus_t apiRetStatus )
{
	Debug();
}
/* This function starts the video streaming application. It is called
 * when there is a SET_INTERFACE event for alternate interface 1
 * (in case of UVC over Isochronous Endpoint usage) or when a
 * COMMIT_CONTROL(SET_CUR) request is received (when using BULK only UVC).
 */
CyU3PReturnStatus_t
CyCx3AppStart (
        void)
{
	Debug ("%s","uvc camera started .......");

    uint8_t SMState = 0;
    CyU3PReturnStatus_t status = CY_U3P_SUCCESS;

    glIsApplnActive = CyTrue;
    glDmaDone       = 0;

#ifdef RESET_TIMER_ENABLE
    CyU3PTimerStop (&Cx3ResetTimer);
#endif

#ifdef STILL_CAPTURE_ENABLE
    glFrameEnd = CyFalse;
#endif

    /* Place the EP in NAK mode before cleaning up the pipe. */
    CyU3PUsbSetEpNak (CX3_EP_BULK_VIDEO, CyTrue);
    CyU3PBusyWait (125);

    /* Reset USB EP and DMA */
    CyU3PUsbFlushEp(CX3_EP_BULK_VIDEO);
    status = CyU3PDmaMultiChannelReset (&glChHandleUVCStream);
    if (status != CY_U3P_SUCCESS)
    {
    	Debug ("AplnStrt:ChannelReset Err = 0x%x", status);
        return status;
    }

    status = CyU3PDmaMultiChannelSetXfer (&glChHandleUVCStream, 0, 0);
    if (status != CY_U3P_SUCCESS)
    {
    	Debug ("AplnStrt:SetXfer Err = 0x%x", status);
        return status;
    }

    CyU3PUsbSetEpNak (CX3_EP_BULK_VIDEO, CyFalse);
    CyU3PBusyWait (200);

    CyU3PUsbLPMDisable ();
    if (CyU3PUsbGetSpeed () == CY_U3P_SUPER_SPEED)
    {
        CyU3PUsbSetLinkPowerState (CyU3PUsbLPM_U0);
    }

    /* Start the GPIF state machine from the start state. */
    CyU3PGpifSMSwitch(CX3_INVALID_GPIF_STATE, CX3_START_SCK0,
            CX3_INVALID_GPIF_STATE, ALPHA_CX3_START_SCK0, CX3_GPIF_SWITCH_TIMEOUT);

    CyU3PThreadSleep (10);
    CyU3PGpifGetSMState(&SMState);
    Debug ("AplnStrt:SMState = 0x%x",SMState);
    CyU3PThreadSleep (10);

    /* Wake Mipi interface and Image Sensor */
    CyU3PMipicsiWakeup();
    CyCx3_ImageSensor_Wakeup();
    glMipiActive = CyTrue;

#ifdef RESET_TIMER_ENABLE
    CyU3PTimerModify (&Cx3ResetTimer, TIMER_PERIOD, 0);
    CyU3PTimerStart (&Cx3ResetTimer);
#endif

    CyCx3_ImageSensor_Trigger_Autofocus();
    return CY_U3P_SUCCESS;
}

/* This function stops the video streaming. It is called from the USB event
 * handler, when there is a reset / disconnect or SET_INTERFACE for alternate
 * interface 0 in case of ischronous implementation or when a Clear Feature (Halt)
 * request is recieved (in case of bulk only implementation).
 */
void
CyCx3AppStop (
        void)
{
	Debug ("%s","uvc camera stoped ...");
#ifdef CX3_DEBUG_ENABLED
    uint8_t SMState = 0;
#endif

    /* Stop the image sensor and CX3 mipi interface */
    CyU3PMipicsiSleep();
    CyCx3_ImageSensor_Sleep();

    glMipiActive = CyFalse;
    glIsStreamingStarted = CyFalse;
#ifdef RESET_TIMER_ENABLE
    CyU3PTimerStop (&Cx3ResetTimer);
#endif

#ifdef CX3_DEBUG_ENABLED
    CyU3PGpifGetSMState(&SMState);
    Debug ( "AplnStop:SMState = 0x%x",SMState);
#endif

    /* Disable the GPIF interface. */
    CyU3PGpifDisable (CyFalse);

    /* Update the flag so that the application thread is notified of this. */
    glIsApplnActive = CyFalse;

    CyU3PUsbSetEpNak (CX3_EP_BULK_VIDEO, CyTrue);
    CyU3PBusyWait (125);

    /* Abort and destroy the video streaming channel */
    /* Reset the channel: Set to DSCR chain starting point in PORD/CONS SCKT; set DSCR_SIZE field in DSCR memory*/
    CyU3PDmaMultiChannelReset(&glChHandleUVCStream);
    CyU3PThreadSleep(25);

    /* Flush the endpoint memory */
    CyU3PUsbFlushEp(CX3_EP_BULK_VIDEO);
    CyU3PUsbSetEpNak (CX3_EP_BULK_VIDEO, CyFalse);
    CyU3PBusyWait (200);

    /* Clear the stall condition and sequence numbers if ClearFeature. */
    if (glIsClearFeature)
    {
        CyU3PUsbStall (CX3_EP_BULK_VIDEO, CyFalse, CyTrue);
        glIsClearFeature = CyFalse;
    }

    glDmaDone = 0;

    /* Enable USB LPM */
    CyU3PUsbLPMEnable ();
}

/* GpifCB callback function is invoked when FV triggers GPIF interrupt */
void
CyCx3AppGpifCB (
        uint8_t currentState)
{
	Debug ();
    /* Handle interrupt from the State Machine */
    switch (currentState)
    {
        case CX3_PARTIAL_BUFFER_IN_SCK0:
            {
                /* Set the wrap-up bit on GPIF SCK0. */
                CyU3PDmaSocketSetWrapUp (CY_U3P_PIB_SOCKET_0);
            }
            break;

        case CX3_PARTIAL_BUFFER_IN_SCK1:
            {
                /* Set the wrap-up bit on GPIF SCK0. */
                CyU3PDmaSocketSetWrapUp (CY_U3P_PIB_SOCKET_1);
            }
            break;

        default:
            break;
    }

#ifdef STILL_CAPTURE_ENABLE
    /* In case of still capture, do not continue the GPIF operation here. It will be started
     * through CyCx3AppStart. */
    if (glStillFlag)
        return;
#endif

#if ((CYFX_VERSION_MINOR > 3) || (CYFX_VERSION_PATCH > 3))
    CyU3PGpifControlSWInput (CyTrue);
    CyU3PGpifControlSWInput (CyFalse);
#else
    /* If we stopped on socket 0, start with socket 1 and vice versa. */
    if ((currentState == CX3_PARTIAL_BUFFER_IN_SCK0) || (currentState == CX3_FULL_BUFFER_IN_SCK0))
        CyU3PGpifSMSwitch(CX3_INVALID_GPIF_STATE, CX3_START_SCK1,
                CX3_INVALID_GPIF_STATE, ALPHA_CX3_START_SCK1, CX3_GPIF_SWITCH_TIMEOUT);
    else
        CyU3PGpifSMSwitch(CX3_INVALID_GPIF_STATE, CX3_START_SCK0,
                CX3_INVALID_GPIF_STATE, ALPHA_CX3_START_SCK0, CX3_GPIF_SWITCH_TIMEOUT);
#endif
}

/* DMA callback function to handle the produce and consume events. */
void
CyCx3AppDmaCallback (
        CyU3PDmaMultiChannel *chHandle,
        CyU3PDmaCbType_t      type,
        CyU3PDmaCBInput_t    *input)
{
    CyU3PDmaBuffer_t dmaBuffer;
    CyU3PReturnStatus_t status = CY_U3P_SUCCESS;

    if (type == CY_U3P_DMA_CB_PROD_EVENT)
    {
        /* This is a produce event notification to the CPU. This notification is
         * received upon reception of every buffer. The buffer will not be sent
         * out unless it is explicitly committed. The call shall fail if there
         * is a bus reset / usb disconnect or if there is any application error. */

        status = CyU3PDmaMultiChannelGetBuffer(chHandle, &dmaBuffer, CYU3P_NO_WAIT);
        while (status == CY_U3P_SUCCESS)
        {
            /* Add Headers*/
            if (dmaBuffer.count < CX3_APP_DATA_BUF_SIZE)
            {
                CyCx3AppAddHeader ((dmaBuffer.buffer - CX3_APP_PROD_HEADER), CX3_APP_HEADER_EOF);

#ifdef RESET_TIMER_ENABLE
                /* We got another end of frame packet. Restart the timer here. */
                CyU3PTimerStop (&Cx3ResetTimer);
                CyU3PTimerModify (&Cx3ResetTimer, TIMER_PERIOD, 0);
                CyU3PTimerStart (&Cx3ResetTimer);
#endif

#ifdef STILL_CAPTURE_ENABLE
                /* If we have a still capture pending, set a flag that indicates that last part of frame has been
                 * captured.
                 */
                if (glStillFlag)
                    glFrameEnd = CyTrue;
#endif

            }
            else
            {
                CyCx3AppAddHeader ((dmaBuffer.buffer - CX3_APP_PROD_HEADER), CX3_APP_HEADER_FRAME);
            }

            /* Commit Buffer to USB*/
            status = CyU3PDmaMultiChannelCommitBuffer (chHandle, (dmaBuffer.count + 12), 0);
            if (status != CY_U3P_SUCCESS)
            {
                CyU3PEventSet(&glCx3Event, CX3_DMA_RESET_EVENT, CYU3P_EVENT_OR);
                break;
            }
            else
            {
                glDmaDone++;
            }

            status = CyU3PDmaMultiChannelGetBuffer(chHandle, &dmaBuffer, CYU3P_NO_WAIT);
        }
    }
    else if (type == CY_U3P_DMA_CB_CONS_EVENT)
    {
        glDmaDone--;
        glIsStreamingStarted = CyTrue;
        glFailFrameCount     = 0;

#ifdef STILL_CAPTURE_ENABLE
        /* In case of still capture, we need to figure out where the frame ends and send an event to the thread. */
        if (glStillFlag == CyTrue)
        {
            if ((glFrameEnd) && (glDmaDone == 0))
            {
            	Debug ("%s","Frame complete");
                CyU3PEventSet (&glStillImageEvent, CX3_STILL_IMAGE_EVENT, CYU3P_EVENT_OR);
            }
        }
#endif
    }
}


/* This is the Callback function to handle the USB Events */
static void
CyCx3AppUSBEventCB (
        CyU3PUsbEventType_t evtype,     /* Event type */
        uint16_t            evdata      /* Event data */
        )
{
    uint8_t interface = 0, altSetting = 0;

    switch (evtype)
    {
        case CY_U3P_USB_EVENT_SUSPEND:
            /* Suspend the device with Wake On Bus Activity set */
            glIsStreamingStarted = CyFalse;
            CyU3PEventSet (&glCx3Event, CX3_USB_SUSP_EVENT_FLAG, CYU3P_EVENT_OR);
            break;

        case CY_U3P_USB_EVENT_SETINTF:
            /* Start the video streamer application if the
             * interface requested was 1. If not, stop the
             * streamer. */
            interface = CY_U3P_GET_MSB(evdata);
            altSetting = CY_U3P_GET_LSB(evdata);

            /* Make sure that the endpoint toggles/sequence numbers are cleared. */
            glIsClearFeature = CyTrue;

#if CX3_DEBUG_ENABLED
            Debug("UsbCB: IF = %d, ALT = %d", interface, altSetting);
#endif
            glIsStreamingStarted = CyFalse;
            if ((altSetting == CX3_APP_STREAM_INTERFACE) && (interface == 1))
            {
                /* Stop the application before re-starting. */
                if (glIsApplnActive)
                {
#if CX3_DEBUG_ENABLED
                	Debug ("%s", "UsbCB:Call AppStop");
#endif
                    CyCx3AppStop ();
                }

                CyCx3AppStart ();
                break;
            }

        /* Intentional Fall-through all cases */
        case CY_U3P_USB_EVENT_SETCONF:
        case CY_U3P_USB_EVENT_RESET:
        case CY_U3P_USB_EVENT_DISCONNECT:
        case CY_U3P_USB_EVENT_CONNECT:
            glIsStreamingStarted = CyFalse;
            if (evtype == CY_U3P_USB_EVENT_SETCONF)
                glIsConfigured = CyTrue;
            else
                glIsConfigured = CyFalse;

            /* Stop the video streamer application and enable LPM. */
            CyU3PUsbLPMEnable ();
            if (glIsApplnActive)
            {
#if CX3_DEBUG_ENABLED
            	Debug ("%s","UsbCB:Call AppStop");
#endif
                CyCx3AppStop ();
            }
            break;

        default:
            break;
    }
}

/* Callback for LPM requests. Always return true to allow host to transition device
 * into required LPM state U1/U2/U3. When data trasmission is active LPM management
 * is explicitly desabled to prevent data transmission errors.
 */
static CyBool_t
CyCx3AppLPMRqtCB (
        CyU3PUsbLinkPowerMode link_mode         /*USB 3.0 linkmode requested by Host */
        )
{
    return CyTrue;
}

#ifdef STILL_CAPTURE_ENABLE

/*	Set the still image resolutions through this function. This function lists all the
 *  supported resolutions in SuperSpeed and HighSpeed. The frame index of resolutions
 *  supported in Still Capture can be different from the frame index of resolutions supported
 *  in Video streaming.
 */
static void
CyCx3AppImageSensorSetStillResolution(
        uint8_t resolution_index
        )
{
    CyU3PReturnStatus_t status = CY_U3P_SUCCESS;
    switch (CyU3PUsbGetSpeed ())
    {
        case CY_U3P_SUPER_SPEED:
            {
                switch (resolution_index)
                {
                    case 0x01:
                        {
                            status = CyU3PMipicsiSetIntfParams (&cfgUvc5Mp15NoMclk, CyFalse);
                            if (status != CY_U3P_SUCCESS)
                            {
                            	Debug ("USBStpCB:SetIntfParams SS2 Err = 0x%x", status);
                            }
                            Debug("%s","CyCx3_ImageSensor_Set_5M");
                            CyCx3_ImageSensor_Set_5M();
                        }
                        break;

                    case 0x02:
                        {
                            /* Write 1080pSettings */
                            status = CyU3PMipicsiSetIntfParams (&cfgUvc1080p30NoMclk, CyFalse);
                            if (status != CY_U3P_SUCCESS)
                            {
                            	Debug ("USBStpCB:SetIntfParams SS1 Err = 0x%x", status);
                            }
                            Debug("%s","CyCx3_ImageSensor_Set_1080p");
                            CyCx3_ImageSensor_Set_1080p ();
                        }
                        break;

                    case 0x03:
                        {
                            /* Write 720pSettings */
                            status = CyU3PMipicsiSetIntfParams (&cfgUvc720p60NoMclk, CyFalse);
                            if (status != CY_U3P_SUCCESS)
                            {
                            	Debug ("USBStpCB:SetIntfParams SS2 Err = 0x%x", status);
                            }
                            Debug("%s","CyCx3_ImageSensor_Set_720p");
                            CyCx3_ImageSensor_Set_720p();
                        }
                        break;

                    case 0x04:
                        {
                            /* Write VGA Settings */
                            status = CyU3PMipicsiSetIntfParams (&cfgUvcVgaNoMclk, CyFalse);
                            if (status != CY_U3P_SUCCESS)
                            {
                            	Debug ("USBStpCB:SetIntfParams HS Err = 0x%x", status);
                            }
                            Debug("%s","CyCx3_ImageSensor_Set_Vga");
                            CyCx3_ImageSensor_Set_Vga();
                        }
                        break;

                    default:
                        break;
                }
            }
            break;

        case CY_U3P_HIGH_SPEED:
            switch (resolution_index)
            {
                case 0x01:
                    {
                        /* Write VGA Settings */
                        status = CyU3PMipicsiSetIntfParams (&cfgUvcVgaNoMclk, CyFalse);
                        if (status != CY_U3P_SUCCESS)
                        {
                        	Debug ("USBStpCB:SetIntfParams HS Err = 0x%x", status);
                        }
                        Debug("%s","CyCx3_ImageSensor_Set_Vga");
                        CyCx3_ImageSensor_Set_Vga();
                    }
                    break;
            }
            break;

        default:
            {
                /* Full speed stream. */
                /* Write VGA Settings */
                status = CyU3PMipicsiSetIntfParams (&cfgUvcVgaNoMclk, CyFalse);
                if (status != CY_U3P_SUCCESS)
                {
                	Debug ("USBStpCB:SetIntfParams FS Err = 0x%x", status);
                }
                Debug("%s","CyCx3_ImageSensor_Set_Vga");
                CyCx3_ImageSensor_Set_Vga();
            }
            break;
    }
}

#endif

/*  Set the video resolution through this function. This function lists all the
 *  supported resolutions in SuperSpeed and HighSpeed. The frame index of resolutions
 *  supported in Still Capture can be different from the frame index of resolutions supported
 *  in Video streaming.
 */
static void
CyCx3AppImageSensorSetVideoResolution(
        uint8_t resolutionIndex
        )
{
    CyU3PReturnStatus_t status = CY_U3P_SUCCESS;
    switch (CyU3PUsbGetSpeed ())
    {
        case CY_U3P_SUPER_SPEED:
            {
                switch (resolutionIndex)
                {
                    case 0x01:
                        {
                            /* Write 1080pSettings */
                            status = CyU3PMipicsiSetIntfParams (&cfgUvc1080p30NoMclk, CyFalse);
                            if (status != CY_U3P_SUCCESS)
                            {
                            	Debug ("USBStpCB:SetIntfParams SS1 Err = 0x%x", status);
                            }

                            CyCx3_ImageSensor_Set_1080p ();
                        }
                        break;

                    case 0x02:
                        {
                            status = CyU3PMipicsiSetIntfParams (&cfgUvc5Mp15NoMclk, CyFalse);
                            if (status != CY_U3P_SUCCESS)
                            {
                            	Debug ("USBStpCB:SetIntfParams SS2 Err = 0x%x", status);
                            }

                            CyCx3_ImageSensor_Set_5M();
                        }
                        break;

                    case 0x03:
                        {
                            /* Write 720pSettings */
                            status = CyU3PMipicsiSetIntfParams (&cfgUvc720p60NoMclk, CyFalse);
                            if (status != CY_U3P_SUCCESS)
                            {
                            	Debug("USBStpCB:SetIntfParams SS2 Err = 0x%x", status);
                            }

                            CyCx3_ImageSensor_Set_720p();
                        }
                        break;

                    case 0x04:
                        {
                            /* Write VGA Settings */
                            status = CyU3PMipicsiSetIntfParams (&cfgUvcVgaNoMclk, CyFalse);
                            if (status != CY_U3P_SUCCESS)
                            {
                            	Debug("USBStpCB:SetIntfParams HS Err = 0x%x", status);
                            }

                            CyCx3_ImageSensor_Set_Vga();
                        }
                        break;
                }
            }
            break;

        case CY_U3P_HIGH_SPEED:
            {
                switch (resolutionIndex)
                {
                    case 0x01:
                        {
                            /* Write VGA Settings */
                            status = CyU3PMipicsiSetIntfParams (&cfgUvcVgaNoMclk, CyFalse);
                            if (status != CY_U3P_SUCCESS)
                            {
                            	Debug("USBStpCB:SetIntfParams HS Err = 0x%x", status);
                            }

                            CyCx3_ImageSensor_Set_Vga();
                        }
                        break;
                }
            }
            break;

        default:
            {
                /* Full speed stream. */
                /* Write VGA Settings */
                status = CyU3PMipicsiSetIntfParams (&cfgUvcVgaNoMclk, CyFalse);
                if (status != CY_U3P_SUCCESS)
                {
                	Debug("USBStpCB:SetIntfParams FS Err = 0x%x", status);
                }

                CyCx3_ImageSensor_Set_Vga();
            }
            break;
    }
}


/* Handles the Probe Control and Commit Control UVC requests. */
static void
CyCx3AppHandleSetCurReq (
        uint16_t wValue
        )
{
    CyU3PReturnStatus_t status;
    uint16_t readCount = 0;

    /* Get the UVC probe/commit control data from EP0 */
    status = CyU3PUsbGetEP0Data(CX3_APP_MAX_PROBE_SETTING_ALIGNED, glCommitCtrl, &readCount);
    if (status != CY_U3P_SUCCESS)
    {
        Debug("USB Setup CB:SET_CUR:GetEP0Data Err = 0x%x", status);
        return;
    }

    /* Check the read count. Expecting a count of CX3_APP_MAX_PROBE_SETTING bytes. */
    if (readCount > (uint16_t)CX3_APP_MAX_PROBE_SETTING)
    {
        Debug("USB Setup CB:Invalid SET_CUR Rqt Len");
        return;
    }

    /* Set Probe Control */
    if (wValue == CX3_APP_VS_PROBE_CONTROL)
    {
        glCurrentFrameIndex = glCommitCtrl[3];
    }
    else
    {
        /* Set Commit Control and Start Streaming*/
        if (wValue == CX3_APP_VS_COMMIT_CONTROL)
        {
            CyCx3AppImageSensorSetVideoResolution (glCommitCtrl[3]);

            if (glIsApplnActive)
            {
#ifdef CX3_DEBUG_ENABLED
                Debug("%s","USB Setup CB:Call AppSTOP1");
#endif
                CyCx3AppStop();
            }

            CyCx3AppStart();
        }
    }
}


/*Returns the pointer to the Probe Control structure for the corresponding frame index.*/
uint8_t *
CyCx3AppGetProbeControlData (
        CyU3PUSBSpeed_t usbConType,
        uint8_t         frameIndex
        )
{
    if (usbConType == CY_U3P_SUPER_SPEED)
    {
        if (frameIndex == 1)
        {
        	/* 1920 x 1080 @30 fps */
            return ((uint8_t *) gl1080pProbeCtrl);
        }
        else if (frameIndex == 2)
        {
        	/* 2952 x 1944 @15 fps */
            return ((uint8_t *)gl5MpProbeCtrl);
        }
        else if (frameIndex == 3)
        {
        	/* 1280 x 720 @60 fps */
            return ((uint8_t *) gl720pProbeCtrl);
        }
        else if (frameIndex == 4)
        {
            /* 640 x 480 @30 fps */
            return ((uint8_t *)glVga30ProbeCtrl);
        }
    }
    else if (usbConType == CY_U3P_HIGH_SPEED)
    {
        if (frameIndex == 1)
        {
            /* 640 x 480 @30 fps */
            return ((uint8_t *)glVga30ProbeCtrl_HS);
        }
        else if (frameIndex == 2)
        {
            /* 640 x 480 @60 fps */
            return ((uint8_t *)glVga60ProbeCtrl);
        }
    }
    else
    {
        if (frameIndex == 1)
        {
            /* 640 x 480 @5 fps */
            return ((uint8_t *)glVga30ProbeCtrl);
        }
    }

    return NULL;
}


/* Callback to handle the USB Setup Requests and UVC Class events */
static CyBool_t
CyCx3AppUSBSetupCB (
        uint32_t setupdat0,     /* SETUP Data 0 */
        uint32_t setupdat1      /* SETUP Data 1 */
        )
{
	Debug();

    CyU3PReturnStatus_t status = CY_U3P_SUCCESS;
    uint8_t   bRequest, bType,bRType, bTarget;
    uint16_t  wValue, wIndex;
    uint8_t   ep0Buf[2];
    uint8_t   temp = 0;
    CyBool_t  isHandled = CyFalse;
    uint8_t   *ctrl_src = 0;

#ifdef STILL_CAPTURE_ENABLE
    uint32_t  eventFlag;
    uint16_t  readCount = 0;
#endif

#if CX3_DEBUG_ENABLED
    uint16_t wLength;
#endif

    /* Decode the fields from the setup request. */
    bRType   = (setupdat0 & CY_U3P_USB_REQUEST_TYPE_MASK);
    bType    = (bRType & CY_U3P_USB_TYPE_MASK);
    bTarget  = (bRType & CY_U3P_USB_TARGET_MASK);
    bRequest = ((setupdat0 & CY_U3P_USB_REQUEST_MASK) >> CY_U3P_USB_REQUEST_POS);
    wValue   = ((setupdat0 & CY_U3P_USB_VALUE_MASK)   >> CY_U3P_USB_VALUE_POS);
    wIndex   = ((setupdat1 & CY_U3P_USB_INDEX_MASK)   >> CY_U3P_USB_INDEX_POS);

#if CX3_DEBUG_ENABLED
    wLength  = ((setupdat1 & CY_U3P_USB_LENGTH_MASK)  >> CY_U3P_USB_LENGTH_POS);
    Debug("bRType = 0x%x, bRequest = 0x%x, wValue = 0x%x, wIndex = 0x%x, wLength= 0x%x",
            bRType, bRequest, wValue, wIndex, wLength);
#endif

    /* ClearFeature(Endpoint_Halt) received on the Streaming Endpoint. Stop Streaming */
    if((bTarget == CY_U3P_USB_TARGET_ENDPT) && (bRequest == CY_U3P_USB_SC_CLEAR_FEATURE)
            && (wIndex == CX3_EP_BULK_VIDEO) && (wValue == CY_U3P_USBX_FS_EP_HALT))
    {
        if ((glIsApplnActive) && (glIsStreamingStarted))
        {
            glIsClearFeature = CyTrue;
            CyCx3AppStop();
        }

        return CyFalse;
    }

    if(bRType == CY_U3P_USB_GS_DEVICE)
    {
        /* Make sure that we bring the link back to U0, so that the ERDY can be sent. */
        if (CyU3PUsbGetSpeed () == CY_U3P_SUPER_SPEED)
            CyU3PUsbSetLinkPowerState (CyU3PUsbLPM_U0);
    }

    if ((bTarget == CY_U3P_USB_TARGET_INTF) && ((bRequest == CY_U3P_USB_SC_SET_FEATURE)
                || (bRequest == CY_U3P_USB_SC_CLEAR_FEATURE)) && (wValue == 0))
    {
#if CX3_DEBUG_ENABLED
        Debug("StpCB:In SET_FTR %d::%d", glIsApplnActive, glIsConfigured);
#endif
        if (glIsConfigured)
        {
            CyU3PUsbAckSetup ();
        }
        else
        {
            CyU3PUsbStall (0, CyTrue, CyFalse);
        }

        return CyTrue;
    }

    if ((bRequest == CY_U3P_USB_SC_GET_STATUS) && (bTarget == CY_U3P_USB_TARGET_INTF))
    {
        /* We support only interface 0. */
        if (wIndex == 0)
        {
            ep0Buf[0] = 0;
            ep0Buf[1] = 0;
            CyU3PUsbSendEP0Data (0x02, ep0Buf);
        }
        else
            CyU3PUsbStall (0, CyTrue, CyFalse);
        return CyTrue;
    }

    /* Check for UVC Class Requests */
    if (bType == CY_U3P_USB_CLASS_RQT)
    {
        /* Requests to the Video Streaming Interface (IF 1) */
        if ((CY_U3P_GET_LSB (wIndex)) == CX3_APP_STREAM_INTERFACE)
        {
            if((wValue == CX3_APP_VS_PROBE_CONTROL) || (wValue == CX3_APP_VS_COMMIT_CONTROL))
            {
                switch (bRequest)
                {
                    case CX3_USB_APP_GET_INFO_REQ:
                        {
                            ep0Buf[0] = 3;
                            CyU3PUsbSendEP0Data (1, (uint8_t *)ep0Buf);
                            isHandled = CyTrue;
                        }
                        break;

                    case CX3_USB_APP_GET_LEN_REQ:
                        {
                            ep0Buf[0] = CX3_APP_MAX_PROBE_SETTING;
                            CyU3PUsbSendEP0Data (1, (uint8_t *)ep0Buf);
                            isHandled = CyTrue;
                        }
                        break;

                    case CX3_USB_APP_GET_CUR_REQ:
                    case CX3_USB_APP_GET_MIN_REQ:
                    case CX3_USB_APP_GET_MAX_REQ:
                    case CX3_USB_APP_GET_DEF_REQ:
                        {
                            /* Host requests for probe data of 34 bytes (UVC 1.1) or 26 Bytes (UVC1.0). Send it over EP0. */
                            ctrl_src = CyCx3AppGetProbeControlData (CyU3PUsbGetSpeed (), glCurrentFrameIndex);
                            if (ctrl_src != 0)
                            {
                                CyU3PMemCopy (glProbeCtrl, (uint8_t *)ctrl_src, CX3_APP_MAX_PROBE_SETTING);

                                status = CyU3PUsbSendEP0Data(CX3_APP_MAX_PROBE_SETTING, glProbeCtrl);
                                if (status != CY_U3P_SUCCESS)
                                {
                                    Debug("USB Setup CB:GET_CUR:SendEP0Data Err = 0x%x", status);
                                }
                            }
                            else
                            {
                                CyU3PUsbStall (0, CyTrue, CyFalse);
                            }

                            isHandled = CyTrue;
                        }
                        break;

                    case CX3_USB_APP_SET_CUR_REQ:
                        {
                            CyCx3AppHandleSetCurReq (wValue);
                            isHandled = CyTrue;
                        }
                        break;

                    default:
                        isHandled = CyFalse;
                        break;
                }
            }
#ifdef STILL_CAPTURE_ENABLE
            else if((wValue == CX3_APP_VS_STILL_PROBE_CONTROL) || (wValue == CX3_APP_VS_STILL_COMMIT_CONTROL))	
            {
                switch (bRequest)
                {
                    case CX3_USB_APP_GET_CUR_REQ:
                    case CX3_USB_APP_GET_MIN_REQ:
                    case CX3_USB_APP_GET_MAX_REQ:
                        {
                        	Debug("Get cur Still probe index = %d", glStillFrameIndex);
                            glStillProbeCtrl[1] = glStillFrameIndex;

                            status = CyU3PUsbSendEP0Data(CX3_APP_MAX_STILL_PROBE_SETTING, (uint8_t*)glStillProbeCtrl);
                            if(status != CY_U3P_SUCCESS)
                            	Debug("Still CyU3PUsbSendEP0Data Failed 0x%x",status);
                        }
                        break;

                    case CX3_USB_APP_SET_CUR_REQ:
                        {
                            /* Get the UVC probe/commit control data from EP0 */
                            status = CyU3PUsbGetEP0Data(16, glStillCommitCtrl, &readCount);
                            if(status != CY_U3P_SUCCESS)
                            	Debug("Still CyU3PUsbGetEP0Data Failed 0x%x",status);

                            glStillFrameIndex = glStillCommitCtrl[1];
                            Debug("Set cur Still probe index = %d", glStillFrameIndex);
                        }
                        break;
                }
                return CyTrue;
            }
            else if (wValue == CX3_APP_VS_STILL_IMAGE_TRIGGER_CONTROL)
            {
                status = CyU3PUsbGetEP0Data(16, &glStillReq, &readCount);
                if(status != CY_U3P_SUCCESS)
                	Debug("Still CyU3PUsbGetEP0Data Failed 0x%x",status);

                glStillFlag = CyTrue;
                CyU3PEventGet (&glStillImageEvent, CX3_STILL_IMAGE_EVENT, CYU3P_EVENT_OR_CLEAR, &eventFlag,
                        CYU3P_WAIT_FOREVER);
                CyCx3AppStop();

                Debug("Still trig Still probe index = %d", glStillFrameIndex);
                CyCx3AppImageSensorSetStillResolution (glStillFrameIndex);

                glUVCHeader[1] ^= CX3_APP_HEADER_STI;
                CyCx3AppStart ();
                Debug("%s","Still Frame start");

                CyU3PEventGet (&glStillImageEvent, CX3_STILL_IMAGE_EVENT, CYU3P_EVENT_OR_CLEAR,
                        &eventFlag, CYU3P_WAIT_FOREVER);

                Debug("%s","Still Frame end");
                CyCx3AppStop ();
                glUVCHeader[1] ^= CX3_APP_HEADER_STI;
                glStillFlag = CyFalse;

                CyCx3AppImageSensorSetVideoResolution (glCurrentFrameIndex);
                CyCx3AppStart ();
                return CyTrue;
            }
#endif
        }
        /* Request addressed to the Video Control Interface */
        else if (CY_U3P_GET_LSB(wIndex) == CX3_APP_CONTROL_INTERFACE)
        {
            /* Respond to VC_REQUEST_ERROR_CODE_CONTROL and stall every other request as this example does
               not support any of the Video Control features */
            if ((wValue == CX3_APP_VC_REQUEST_ERROR_CODE_CONTROL) && (wIndex == 0x00))
            {
                temp = CX3_APP_ERROR_INVALID_CONTROL;
                status = CyU3PUsbSendEP0Data(0x01, &temp);
                if (status != CY_U3P_SUCCESS)
                {
                    Debug("USBStpCB:VCI SendEP0Data = %d", status);
                }

                isHandled = CyTrue;
            }
        }
    }

    return isHandled;
}


/* Callback function to handle LPM requests from the USB 3.0 host. This function is invoked by the API
   whenever a state change from U0 -> U1 or U0 -> U2 happens. If we return CyTrue from this function, the
   FX3 device is retained in the low power state. If we return CyFalse, the FX3 device immediately tries
   to trigger an exit back to U0.

   This application does not have any state in which we should not allow U1/U2 transitions; and therefore
   the function always return CyTrue.
 */
CyBool_t
CyFxUsbHidApplnLPMRqtCB (
        CyU3PUsbLinkPowerMode link_mode)
{
    return CyTrue;
}

/* This is the callback function to handle the USB events. */
void
CyFxUsbHidApplnUSBEventCB (
        CyU3PUsbEventType_t evtype, /* Event type */
        uint16_t            evdata  /* Event data */
        )
{
    switch (evtype)
    {
        case CY_U3P_USB_EVENT_SETCONF:
            /* Disable the low power entry to optimize USB throughput */
            CyU3PUsbLPMDisable();

            /* Stop application before re-starting. */
            if (glIsApplnActive)
            {
                CyFxUsbHidApplnStop ();
            }
            /* Start application. */
            CyFxUsbHidApplnStart ();
            break;

        case CY_U3P_USB_EVENT_RESET:
        case CY_U3P_USB_EVENT_DISCONNECT:
            /* Stop application. */
            if (glIsApplnActive)
            {
                CyFxUsbHidApplnStop ();
            }
            break;

        default:
            break;
    }
}

/* Callback to handle the USB setup requests. */
CyBool_t
CyFxUsbHidApplnUSBSetupCB (
        uint32_t setupdat0, /* SETUP Data 0 */
        uint32_t setupdat1  /* SETUP Data 1 */
        )
{
    /* Fast enumeration is used. Only requests addressed to the interface, class,
     * vendor and unknown control requests are received by this function.
     * This application does not support any class or vendor requests. */

    uint8_t  bRequest, bReqType;
    uint8_t  bType, bTarget;
    uint16_t wValue, wIndex;
    CyBool_t isHandled = CyFalse;
    CyU3PReturnStatus_t status = CY_U3P_SUCCESS;

    /* Decode the fields from the setup request. */
    bReqType = (setupdat0 & CY_U3P_USB_REQUEST_TYPE_MASK);
    bType    = (bReqType & CY_U3P_USB_TYPE_MASK);
    bTarget  = (bReqType & CY_U3P_USB_TARGET_MASK);
    bRequest = ((setupdat0 & CY_U3P_USB_REQUEST_MASK) >> CY_U3P_USB_REQUEST_POS);
    wValue   = ((setupdat0 & CY_U3P_USB_VALUE_MASK)   >> CY_U3P_USB_VALUE_POS);
    wIndex   = ((setupdat1 & CY_U3P_USB_INDEX_MASK)   >> CY_U3P_USB_INDEX_POS);

    if (bType == CY_U3P_USB_STANDARD_RQT)
    {
        /* Handle SET_FEATURE(FUNCTION_SUSPEND) and CLEAR_FEATURE(FUNCTION_SUSPEND)
         * requests here. It should be allowed to pass if the device is in configured
         * state and failed otherwise. */
        if ((bTarget == CY_U3P_USB_TARGET_INTF) && ((bRequest == CY_U3P_USB_SC_SET_FEATURE)
                    || (bRequest == CY_U3P_USB_SC_CLEAR_FEATURE)) && (wValue == 0))
        {
            if (glIsApplnActive)
                CyU3PUsbAckSetup ();
            else
                CyU3PUsbStall (0, CyTrue, CyFalse);

            isHandled = CyTrue;
        }

        if ((bTarget == CY_U3P_USB_TARGET_ENDPT) && (bRequest == CY_U3P_USB_SC_CLEAR_FEATURE)
                && (wValue == CY_U3P_USBX_FS_EP_HALT))
        {
            if (wIndex == CY_FX_HID_EP_INTR_IN)
            {
                if (glIsApplnActive)
                {
                    CyU3PUsbSetEpNak (CY_FX_HID_EP_INTR_IN, CyTrue);
                    CyU3PBusyWait (125);

                   // CyU3PDmaChannelReset (&glChHandleIntrCPU2U);
                    CyU3PDmaChannelReset (&glChHandleUVCStream);
                    CyU3PUsbFlushEp (CY_FX_HID_EP_INTR_IN);
                    CyU3PUsbResetEp (CY_FX_HID_EP_INTR_IN);
                    CyU3PUsbStall (wIndex, CyFalse, CyTrue);

                    CyU3PUsbSetEpNak (CY_FX_HID_EP_INTR_IN, CyFalse);

                    CyU3PUsbAckSetup ();
                    isHandled = CyTrue;
                }
            }
        }

        /* Class specific descriptors such as HID Report descriptor need to handled by the callback. */
        bReqType = ((setupdat0 & CY_U3P_USB_VALUE_MASK) >> 24);
        if ((bRequest == CY_U3P_USB_SC_GET_DESCRIPTOR) && (bReqType == CY_FX_GET_REPORT_DESC))
        {
            isHandled = CyTrue;

            status = CyU3PUsbSendEP0Data (0x1C, (uint8_t *)CyFxUSBReportDscr);
            if (status != CY_U3P_SUCCESS)
            {
                /* There was some error. We should try stalling EP0. */
                CyU3PUsbStall(0, CyTrue, CyFalse);
            }
        }
    }
    else if (bType == CY_U3P_USB_CLASS_RQT)
    {
        /* Class Specific Request Handler */
        if (bRequest == CY_FX_HID_SET_IDLE)
        {
            CyU3PUsbAckSetup ();
           // glButtonPress = 0;
            isHandled = CyTrue;
        }
    }

    return isHandled;
}

void CyCx3UsbAppInit()
{
	Debug("begin init usb hid ......");
	CyU3PReturnStatus_t apiRetStatus = CY_U3P_SUCCESS;

	/* Register callbacks for handling USB events and requests. */

	CyU3PUsbRegisterSetupCallback(CyFxUsbHidApplnUSBSetupCB, CyTrue);

	CyU3PUsbRegisterLPMRequestCallback(CyFxUsbHidApplnLPMRqtCB);

	CyU3PUsbRegisterEventCallback(CyFxUsbHidApplnUSBEventCB);

	/* Register the USB descriptors with the driver. */

	/* Super speed device descriptor. */
	Debug("Super speed device descriptor. ");
	apiRetStatus = CyU3PUsbSetDesc(CY_U3P_USB_SET_SS_DEVICE_DESCR, 0, (uint8_t *)CyFxUSB30DeviceDscr);
	if (apiRetStatus != CY_U3P_SUCCESS)
	{
		CyU3PDebugPrint (4, "USB set device descriptor failed, Error code = %d\n", apiRetStatus);
		CyFxAppErrorHandler(apiRetStatus);
	}

	/* High speed device descriptor. */
	Debug("High speed device descriptor. ");
	apiRetStatus = CyU3PUsbSetDesc(CY_U3P_USB_SET_HS_DEVICE_DESCR, 0, (uint8_t *)CyFxUSB20DeviceDscr);
	if (apiRetStatus != CY_U3P_SUCCESS)
	{
		CyU3PDebugPrint (4, "USB set device descriptor failed, Error code = %d\n", apiRetStatus);
		CyFxAppErrorHandler(apiRetStatus);
	}

	/* BOS descriptor */
	Debug("BOS descriptor ");
	apiRetStatus = CyU3PUsbSetDesc(CY_U3P_USB_SET_SS_BOS_DESCR, 0, (uint8_t *)CyFxUSBBOSDscr);
	if (apiRetStatus != CY_U3P_SUCCESS)
	{
		CyU3PDebugPrint (4, "USB set configuration descriptor failed, Error code = %d\n", apiRetStatus);
		CyFxAppErrorHandler(apiRetStatus);
	}

	/* Device qualifier descriptor */
	Debug("Device qualifier descriptor  ");
	apiRetStatus = CyU3PUsbSetDesc(CY_U3P_USB_SET_DEVQUAL_DESCR, 0, (uint8_t *)CyFxUSBDeviceQualDscr);
	if (apiRetStatus != CY_U3P_SUCCESS)
	{
		CyU3PDebugPrint (4, "USB set device qualifier descriptor failed, Error code = %d\n", apiRetStatus);
		CyFxAppErrorHandler(apiRetStatus);
	}

	/* Super speed configuration descriptor */
	Debug(" Super speed configuration descriptor");
	apiRetStatus = CyU3PUsbSetDesc(CY_U3P_USB_SET_SS_CONFIG_DESCR, 0, (uint8_t *)CyFxUSBSSConfigDscr);
	if (apiRetStatus != CY_U3P_SUCCESS)
	{
		CyU3PDebugPrint (4, "USB set configuration descriptor failed, Error code = %d\n", apiRetStatus);
		CyFxAppErrorHandler(apiRetStatus);
	}

	/* High speed configuration descriptor */
	Debug("High speed configuration descriptor");
	apiRetStatus = CyU3PUsbSetDesc(CY_U3P_USB_SET_HS_CONFIG_DESCR, 0, (uint8_t *)CyFxUSBHSConfigDscr);
	if (apiRetStatus != CY_U3P_SUCCESS)
	{
		CyU3PDebugPrint (4, "USB Set Other Speed Descriptor failed, Error Code = %d\n", apiRetStatus);
		CyFxAppErrorHandler(apiRetStatus);
	}

	/* Full speed configuration descriptor */
	Debug("Full speed configuration descriptor");
	apiRetStatus = CyU3PUsbSetDesc(CY_U3P_USB_SET_FS_CONFIG_DESCR, 0, (uint8_t *)CyFxUSBFSConfigDscr);
	if (apiRetStatus != CY_U3P_SUCCESS)
	{
		CyU3PDebugPrint (4, "USB Set Configuration Descriptor failed, Error Code = %d\n", apiRetStatus);
		CyFxAppErrorHandler(apiRetStatus);
	}

	/* String descriptor 0 */
	Debug("String descriptor 0");
	apiRetStatus = CyU3PUsbSetDesc(CY_U3P_USB_SET_STRING_DESCR, 0, (uint8_t *)CyFxUSBStringLangIDDscr);
	if (apiRetStatus != CY_U3P_SUCCESS)
	{
		CyU3PDebugPrint (4, "USB set string descriptor failed, Error code = %d\n", apiRetStatus);
		CyFxAppErrorHandler(apiRetStatus);
	}

	/* String descriptor 1 */
	Debug("String descriptor 1");
	apiRetStatus = CyU3PUsbSetDesc(CY_U3P_USB_SET_STRING_DESCR, 1, (uint8_t *)CyFxUSBManufactureDscr);
	if (apiRetStatus != CY_U3P_SUCCESS)
	{
		CyU3PDebugPrint (4, "USB set string descriptor failed, Error code = %d\n", apiRetStatus);
		CyFxAppErrorHandler(apiRetStatus);
	}

	/* String descriptor 2 */
	Debug("String descriptor 2");
	apiRetStatus = CyU3PUsbSetDesc(CY_U3P_USB_SET_STRING_DESCR, 2, (uint8_t *)CyFxUSBProductDscr);
	if (apiRetStatus != CY_U3P_SUCCESS)
	{
		CyU3PDebugPrint (4, "USB set string descriptor failed, Error code = %d\n", apiRetStatus);
		CyFxAppErrorHandler(apiRetStatus);
	}

	/* String descriptor 3 */
	Debug("String descriptor 3");
	apiRetStatus = CyU3PUsbSetDesc(CY_U3P_USB_SET_STRING_DESCR, 3, (uint8_t *)CyFxUSBSerialNumberDscr);
	if (apiRetStatus != CY_U3P_SUCCESS)
	{
		CyU3PDebugPrint (4, "USB set string descriptor failed, Error code = %d\n", apiRetStatus);
		CyFxAppErrorHandler(apiRetStatus);
	}
    Debug("usb inited !!!!");

}


/* This function initialines the USB Module, creates event group,
   sets the enumeration descriptors, configures the Endpoints and
   configures the DMA module for the UVC Application */
void
CyCx3AppInit (
        void)
{
	//Debug(" %s"," begin");
    CyU3PEpConfig_t endPointConfig;
    CyU3PDmaMultiChannelConfig_t dmaCfg;
    CyU3PReturnStatus_t status = CY_U3P_SUCCESS;

#ifdef CX3_DEBUG_ENABLED
    CyU3PMipicsiCfg_t readCfg;
    CyU3PMipicsiErrorCounts_t errCnts;
#endif

    /* Initialize the I2C interface for Mipi Block Usage and Camera. */
   // CyU3PDebugPrint (4, "\n\r%s:%d  Initialize the I2C interface for Mipi Block Usage and Camera", __func__,__LINE__);
    status = CyU3PMipicsiInitializeI2c (CY_U3P_MIPICSI_I2C_400KHZ);
    if(status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:I2CInit Err = 0x%x.",status);
        CyCx3AppErrorHandler(status);
    }

    /* Initialize GPIO module. */
   // CyU3PDebugPrint (4, "\n\r %s:%d Initialize GPIO module",__func__,__LINE__);
    status = CyU3PMipicsiInitializeGPIO ();
    if( status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:GPIOInit Err = 0x%x",status);
        CyCx3AppErrorHandler(status);
    }

    /* Initialize the PIB block */
   // CyU3PDebugPrint (4, "\n\r %s:%d Initialize the PIB block",__func__,__LINE__);
    status = CyU3PMipicsiInitializePIB ();
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:PIBInit Err = 0x%x",status);
        CyCx3AppErrorHandler(status);
    }

    /* Start the USB functionality */
    Debug(" %s:%d CyU3PUsbStart",__func__,__LINE__);
    status = CyU3PUsbStart();
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:UsbStart Err = 0x%x",status);
        CyCx3AppErrorHandler(status);
    }

    /* The fast enumeration is the easiest way to setup a USB connection,
     * where all enumeration phase is handled by the library. Only the
     * class / vendor requests need to be handled by the application. */
    //CyU3PDebugPrint (4, "\n\r %s:%d CyCx3AppUSBSetupCB",__func__,__LINE__);
    CyU3PUsbRegisterSetupCallback(CyCx3AppUSBSetupCB, CyTrue);

    /* Setup the callback to handle the USB events */
    //CyU3PDebugPrint (4, "\n\r %s:%d CyCx3AppUSBEventCB",__func__,__LINE__);
    CyU3PUsbRegisterEventCallback(CyCx3AppUSBEventCB);

    /* Register a callback to handle LPM requests from the USB 3.0 host. */
    //CyU3PDebugPrint (4, "\n\r %s:%d CyCx3AppLPMRqtCB",__func__,__LINE__);
    CyU3PUsbRegisterLPMRequestCallback (CyCx3AppLPMRqtCB);

    /* Set the USB Enumeration descriptors */

    /* Super speed device descriptor. */
    //CyU3PDebugPrint (4, "\n\r %s:%d Super speed device descriptor ",__func__,__LINE__);
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_SS_DEVICE_DESCR, 0, (uint8_t *)CyCx3USB30DeviceDscr);
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:Set_SS_Device_Dscr Err = 0x%x", status);
        CyCx3AppErrorHandler(status);
    }

    /* High speed device descriptor. */
    Debug("High speed device descriptor");
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_HS_DEVICE_DESCR, 0, (uint8_t *)CyCx3USB20DeviceDscr);
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:Set_HS_Device_Dscr Err = 0x%x", status);
        CyCx3AppErrorHandler(status);
    }

    /* BOS descriptor */
    Debug("BOS descriptor" );
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_SS_BOS_DESCR, 0, (uint8_t *)CyCx3USBBOSDscr);
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:Set_BOS_Dscr Err = 0x%x", status);
        CyCx3AppErrorHandler(status);
    }

    /* Device qualifier descriptor */
    Debug("Device qualifier descriptor");
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_DEVQUAL_DESCR, 0, (uint8_t *)CyCx3USBDeviceQualDscr);
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:Set_DEVQUAL_Dscr Err = 0x%x", status);
        CyCx3AppErrorHandler(status);
    }

    /* Super speed configuration descriptor */
    Debug("Super speed configuration descriptor");
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_SS_CONFIG_DESCR, 0, (uint8_t *)CyCx3USBSSConfigDscr);
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:Set_SS_CFG_Dscr Err = 0x%x", status);
        CyCx3AppErrorHandler(status);
    }

    /* High speed configuration descriptor */
    Debug("High speed configuration descriptor ");
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_HS_CONFIG_DESCR, 0, (uint8_t *)CyCx3USBHSConfigDscr);
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:Set_HS_CFG_Dscr Err = 0x%x", status);
        CyCx3AppErrorHandler(status);
    }

    /* Full speed configuration descriptor */
    Debug("Full speed configuration descriptor");
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_FS_CONFIG_DESCR, 0, (uint8_t *)CyCx3USBFSConfigDscr);
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:Set_FS_CFG_Dscr Err = 0x%x", status);
        CyCx3AppErrorHandler(status);
    }

    /* String descriptor 0 */
    Debug("String descriptor 0 ");
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_STRING_DESCR, 0, (uint8_t *)CyCx3USBStringLangIDDscr);
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:Set_STRNG_Dscr0 Err = 0x%x", status);
        CyCx3AppErrorHandler(status);
    }

    /* String descriptor 1 */
    Debug("String descriptor 1");
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_STRING_DESCR, 1, (uint8_t *)CyCx3USBManufactureDscr);
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:Set_STRNG_Dscr1 Err = 0x%x", status);
        CyCx3AppErrorHandler(status);
    }

    /* String descriptor 2 */
    Debug("String descriptor 2");
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_STRING_DESCR, 2, (uint8_t *)CyCx3USBProductDscr);
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:Set_STRNG_Dscr2 Err = 0x%x", status);
        CyCx3AppErrorHandler(status);
    }
    /* String descriptor 3 */
    Debug("String descriptor 3");
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_STRING_DESCR, 3, (uint8_t *)CyCx3USBConfigSSDscr);
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:Set_STRNG_Dscr3 Err = 0x%x", status);
        CyCx3AppErrorHandler(status);
    }

    /* String descriptor 4 */
    Debug("String descriptor 4");
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_STRING_DESCR, 4, (uint8_t *)CyCx3USBConfigHSDscr);
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:Set_STRNG_Dscr4 Err = 0x%x", status);
        CyCx3AppErrorHandler(status);
    }

    /* String descriptor 5 */
    Debug("String descriptor 5");
    status = CyU3PUsbSetDesc(CY_U3P_USB_SET_STRING_DESCR, 5, (uint8_t *)CyCx3USBConfigFSDscr);
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:Set_STRNG_Dscr5 Err = 0x%x", status);
        CyCx3AppErrorHandler(status);
    }

    /* We enable device operation off VBat and use the VBatt signal for USB connection detection. This is the standard
     * setting for all CX3 designs, as the VBus and VBatt signals are connected to a single pad.
     */
    CyU3PUsbVBattEnable (CyTrue);
    CyU3PUsbControlVBusDetect (CyFalse, CyTrue);

    /* Control status interrupt endpoint configuration:
       We are not actually using the endpoint, and only leaving it enabled so that any requests will be NAKed.
     */
    endPointConfig.enable   = 1;
    endPointConfig.epType   = CY_U3P_USB_EP_INTR;
    endPointConfig.isoPkts  = 1;
    endPointConfig.streams  = 0;
    endPointConfig.pcktSize = CX3_EP_INTR_PACKET_SIZE;
    endPointConfig.burstLen = CX3_EP_INTR_BURST_LEN;

    Debug("CyU3PSetEpConfig >>CX3_EP_CONTROL_STATUS");
    status = CyU3PSetEpConfig(CX3_EP_CONTROL_STATUS, &endPointConfig);
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:CyU3PSetEpConfig CtrlEp Err = 0x%x", status);
        CyCx3AppErrorHandler(status);
    }

    /* Setup the Bulk endpoint used for Video Streaming:
       Always configure the endpoint with SuperSpeed parameters. The FX3 library will make adjustments as required.
     */
    endPointConfig.enable   = CyTrue;
    endPointConfig.epType   = CY_U3P_USB_EP_BULK;
    endPointConfig.isoPkts  = 0;
    endPointConfig.streams  = 0;
    endPointConfig.pcktSize = CX3_EP_BULK_VIDEO_PKT_SIZE;
    endPointConfig.burstLen = CX3_EP_BULK_SUPER_SPEED_BURST_LEN;
    Debug("CyU3PSetEpConfig >>CX3_EP_BULK_VIDEO ");
    status = CyU3PSetEpConfig(CX3_EP_BULK_VIDEO, &endPointConfig);
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:CyU3PSetEpConfig BulkEp Err = 0x%x", status);
        CyCx3AppErrorHandler(status);
    }

    CyU3PUsbEPSetBurstMode (CX3_EP_BULK_VIDEO, CyTrue);

    /* Create a DMA Manual OUT channel for streaming data */
    /* Video streaming Channel is not active till a stream request is received */
    dmaCfg.size                 = CX3_APP_STREAM_BUF_SIZE;
    dmaCfg.count                = CX3_APP_STREAM_BUF_COUNT;
    dmaCfg.validSckCount        = CX3_APP_SOCKET_COUNT;
    dmaCfg.prodSckId[0]         = CX3_PRODUCER_PPORT_SOCKET_0;
    dmaCfg.prodSckId[1]         = CX3_PRODUCER_PPORT_SOCKET_1;
    dmaCfg.consSckId[0]         = CX3_EP_VIDEO_CONS_SOCKET;
    dmaCfg.dmaMode              = CY_U3P_DMA_MODE_BYTE;
    dmaCfg.notification         = CY_U3P_DMA_CB_PROD_EVENT | CY_U3P_DMA_CB_CONS_EVENT;
    dmaCfg.cb                   = CyCx3AppDmaCallback;
    dmaCfg.prodHeader           = CX3_APP_PROD_HEADER;
    dmaCfg.prodFooter           = CX3_APP_PROD_FOOTER;
    dmaCfg.consHeader           = 0;
    dmaCfg.prodAvailCount       = 0;

    status = CyU3PDmaMultiChannelCreate (&glChHandleUVCStream, CY_U3P_DMA_TYPE_MANUAL_MANY_TO_ONE , &dmaCfg);
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:DmaMultiChannelCreate Err = 0x%x", status);
    }

    /* Configure the Fixed Function GPIF on the CX3 to use a 16 bit bus, and
     * a DMA Buffer of size CX3_APP_DATA_BUF_SIZE
     */
    status = CyU3PMipicsiGpifLoad(CY_U3P_MIPICSI_BUS_16, CX3_APP_DATA_BUF_SIZE);
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:MipicsiGpifLoad Err = 0x%x", status);
        CyCx3AppErrorHandler(status);
    }

    CyU3PGpifRegisterSMIntrCallback (CyCx3AppGpifCB);

    /* Initialize the MIPI block */
    status =  CyU3PMipicsiInit();
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:MipicsiInit Err = 0x%x", status);
        CyCx3AppErrorHandler(status);
    }

    status = CyU3PMipicsiSetIntfParams(&cfgUvcVgaNoMclk, CyFalse);
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:MipicsiSetIntfParams Err = 0x%x",status);
        CyCx3AppErrorHandler(status);
    }

#ifdef CX3_DEBUG_ENABLED
    status = CyU3PMipicsiQueryIntfParams (&readCfg);
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:MipicsiQueryIntfParams Err = 0x%x",status);
        CyCx3AppErrorHandler(status);
    }

    status = CyU3PMipicsiGetErrors (CyFalse, &errCnts);
#endif

    /* Setup Image Sensor */
    CyCx3_ImageSensor_Init();
    CyCx3_ImageSensor_Sleep();

#ifdef RESET_TIMER_ENABLE
    CyU3PTimerCreate (&Cx3ResetTimer, CyCx3AppProgressTimer, 0x00, TIMER_PERIOD, 0, CYU3P_NO_ACTIVATE);
#endif







    /* Connect the USB pins and enable super speed operation */
    Debug("CyU3PConnectState " );
    status = CyU3PConnectState (CyTrue, CyTrue);
    if (status != CY_U3P_SUCCESS)
    {
        Debug("AppInit:ConnectState Err = 0x%x", status);
        CyCx3AppErrorHandler(status);
    }

    //Debug(" %s","init hid usb devices ");
    //added ....
    //CyCx3UsbAppInit();

   // Debug(" %s","end =====" );
}

/* This function initializes the debug module for the UVC application */
void
CyCx3AppDebugInit (
        void)
{
    CyU3PUartConfig_t uartConfig;
    CyU3PReturnStatus_t status = CY_U3P_SUCCESS;

    /* Errors in starting up the UART and enabling it for debug are not fatal errors.
     * Also, we cannot use DebugPrint until the debug module has been successfully initialized.
     */

    /* Initialize the UART for printing debug messages */
    status = CyU3PUartInit();
    if (status != CY_U3P_SUCCESS)
    {
        return;
    }

    /* Set UART Configuration */
    uartConfig.baudRate = CY_U3P_UART_BAUDRATE_115200;
    uartConfig.stopBit  = CY_U3P_UART_ONE_STOP_BIT;
    uartConfig.parity   = CY_U3P_UART_NO_PARITY;
    uartConfig.txEnable = CyTrue;
    uartConfig.rxEnable = CyFalse;
    uartConfig.flowCtrl = CyFalse;
    uartConfig.isDma    = CyTrue;

    /* Set the UART configuration */
    status = CyU3PUartSetConfig (&uartConfig, NULL);
    if (status != CY_U3P_SUCCESS)
    {
        return;
    }

    /* Set the UART transfer */
    status = CyU3PUartTxSetBlockXfer (0xFFFFFFFF);
    if (status != CY_U3P_SUCCESS)
    {
        return;
    }

    /* Initialize the debug application */
    status = CyU3PDebugInit (CY_U3P_LPP_SOCKET_UART_CONS, 8);
    if (status != CY_U3P_SUCCESS)
    {
        return;
    }

    CyU3PDebugPreamble (CyFalse);
    Debug ("%s"," Ready to roll");
}

/* Entry function for the UVC application thread. */
void
CyCx3AppThread_Entry (
        uint32_t input)
{

    uint16_t wakeReason;
    uint32_t eventFlag;
    uint32_t evMask = CX3_USB_SUSP_EVENT_FLAG | CX3_DMA_RESET_EVENT;
    CyU3PReturnStatus_t status;

    /* Initialize the Debug Module */
    CyCx3AppDebugInit();
    Debug("%s","begin app init ======");
    /* Initialize the UVC Application */
    CyCx3AppInit();

    Debug("%s","app inited ======");

    for (;;)
    {
        eventFlag = 0;
        Debug(" %s","begin CyU3PEventGet ......................");
        status = CyU3PEventGet (&glCx3Event, evMask, CYU3P_EVENT_OR_CLEAR, &eventFlag, CYU3P_WAIT_FOREVER);
        //Debug(" ====  CyU3PEventGet %d",status);
        if (status == CY_U3P_SUCCESS)
        {
            if (eventFlag & CX3_DMA_RESET_EVENT)
            {
            	Debug("%s","DMA RESET");
                /* Frame timed out. Abort and start streaming again. */
                if (glIsApplnActive)
                {
                    CyCx3AppStop();
                }

#ifdef RESET_TIMER_ENABLE
                if (glFailFrameCount < 5)
                    glFailFrameCount++;
                if (glFailFrameCount < 5)
#endif
                {
                    /* If 5 frames fail back-to-back, assume that host application is no longer running. */
                    CyCx3AppStart ();
                }

#ifdef RESET_TIMER_ENABLE
                CyU3PTimerStop (&Cx3ResetTimer);
                CyU3PTimerModify (&Cx3ResetTimer, TIMER_PERIOD, 0);
#endif
            }

            /* Handle Suspend Event*/
            if (eventFlag & CX3_USB_SUSP_EVENT_FLAG)
            {
                /* Place CX3 in Low Power Suspend mode, with USB bus activity as the wakeup source. */
                CyU3PMipicsiSleep();
                CyCx3_ImageSensor_Sleep();

                status = CyU3PSysEnterSuspendMode (CY_U3P_SYS_USB_BUS_ACTVTY_WAKEUP_SRC, 0, &wakeReason);
                Debug("EnterSuspendMode Status =  0x%x, Wakeup reason = 0x%x", status, wakeReason);
                if (glMipiActive)
                {
                    CyU3PMipicsiWakeup();
                    CyCx3_ImageSensor_Wakeup();
                }
            }
        }
        else
        {
            continue;
        }
    }
}

#ifdef CX3_ERROR_THREAD_ENABLE
void
CyCx3AppMipiErrorThread (
        uint32_t input)
{
    uint32_t eventFlag;
    CyU3PMipicsiErrorCounts_t errCnts;

#ifdef CX3_DEBUG_ENABLED
    Debug ("MipiErrorThread Init.");
#endif

    for (;;)
    {
        /* Read Errors every 5 Seconds */
        CyU3PEventGet (&glMipiErrorEvent, CX3_MIPI_ERROR_EVENT, CYU3P_EVENT_OR_CLEAR, &eventFlag, 5000);
        if(glIsApplnActive == CyTrue)
            CyU3PMipicsiGetErrors( CyTrue, &errCnts);
    }
}
#endif


/* Application define function which creates the threads. */
void
CyFxApplicationDefine (
        void)
{
	//CyU3PDebugPrint (4, "\n\r %s:%d",__func__,__LINE__);
    void *ptr = NULL;
    uint32_t apiRetStatus = CY_U3P_SUCCESS;

    /* Allocate the memory for the thread and create the thread */
    ptr = CyU3PMemAlloc (CX3_APP_THREAD_STACK);
    if (ptr == NULL)
        goto StartupError;

    apiRetStatus = CyU3PThreadCreate (&cx3AppThread,    /* UVC Thread structure */
            "30:CX3_app_thread",                        /* Thread Id and name */
            CyCx3AppThread_Entry,                    /* UVC Application Thread Entry function */
            0,                                          /* No input parameter to thread */
            ptr,                                        /* Pointer to the allocated thread stack */
            CX3_APP_THREAD_STACK,                       /* UVC Application Thread stack size */
            CX3_APP_THREAD_PRIORITY,                    /* UVC Application Thread priority */
            CX3_APP_THREAD_PRIORITY,                    /* Pre-emption threshold */
            CYU3P_NO_TIME_SLICE,                        /* No time slice for the application thread */
            CYU3P_AUTO_START                            /* Start the Thread immediately */
            );

    /* Check the return code */
    if (apiRetStatus != CY_U3P_SUCCESS)
        goto StartupError;

    /* Create GPIO application event group */
    if (CyU3PEventCreate(&glCx3Event) != CY_U3P_SUCCESS)
        goto StartupError;

#ifdef STILL_CAPTURE_ENABLE
	/* Create GPIO application event group for still image related events */
	if (CyU3PEventCreate(&glStillImageEvent) != CY_U3P_SUCCESS)
		goto StartupError;
#endif

#ifdef CX3_ERROR_THREAD_ENABLE
    /* Allocate the memory for the thread and create the thread */
    ptr = NULL;
    ptr = CyU3PMemAlloc (CX3_MIPI_ERROR_THREAD_STACK);
    if (ptr == NULL)
        goto StartupError;

    apiRetStatus = CyU3PThreadCreate (&cx3MipiErrorThread,    /* UVC Thread structure */
            "31:CX3_Mipi_Error_thread",                       /* Thread Id and name */
            CyCx3AppMipiErrorThread,                          /* UVC Application Thread Entry function */
            0,                                                /* No input parameter to thread */
            ptr,                                              /* Pointer to the allocated thread stack */
            CX3_MIPI_ERROR_THREAD_STACK,                      /* UVC Application Thread stack size */
            CX3_MIPI_ERROR_THREAD_PRIORITY,                   /* UVC Application Thread priority */
            CX3_MIPI_ERROR_THREAD_PRIORITY,                   /* Pre-emption threshold */
            CYU3P_NO_TIME_SLICE,                              /* No time slice for the application thread */
            CYU3P_AUTO_START                                  /* Start the Thread immediately */
            );

    /* Check the return code */
    if (apiRetStatus != CY_U3P_SUCCESS)
        goto StartupError;

    if (CyU3PEventCreate(&glMipiErrorEvent) != CY_U3P_SUCCESS)
        goto StartupError;
#endif





    return;

StartupError:
    {
        /* Failed to create threads and objects required for the application. This is a fatal error and we cannot
         * continue.
         */

        /* Add custom recovery or debug actions here */

        while(1);
    }
}

/*
 * Main function
 */
int
main (
        void)
{
    CyU3PIoMatrixConfig_t io_cfg;
    CyU3PReturnStatus_t status = CY_U3P_SUCCESS;

    /* Initialize the device */
    status = CyU3PDeviceInit (NULL);
    if (status != CY_U3P_SUCCESS)
    {
        goto handle_fatal_error;
    }

    /* Initialize the caches. Enable instruction cache and keep data cache disabled.
     * The data cache is useful only when there is a large amount of CPU based memory
     * accesses. When used in simple cases, it can decrease performance due to large
     * number of cache flushes and cleans and also it adds to the complexity of the
     * code. */
    status = CyU3PDeviceCacheControl (CyTrue, CyFalse, CyFalse);
    if (status != CY_U3P_SUCCESS)
    {
        goto handle_fatal_error;
    }

    /* Configure the IO matrix for the device.*/
    io_cfg.isDQ32Bit = CyFalse;
    //add
    io_cfg.s0Mode    = CY_U3P_SPORT_INACTIVE;
    io_cfg.s1Mode    = CY_U3P_SPORT_INACTIVE;

    io_cfg.useUart   = CyTrue;
    io_cfg.useI2C    = CyTrue;
    io_cfg.useI2S    = CyFalse;
    io_cfg.useSpi    = CyFalse;
    io_cfg.lppMode   = CY_U3P_IO_MATRIX_LPP_DEFAULT;

    /* No GPIOs are enabled. */
    io_cfg.gpioSimpleEn[0]  = 0;
    io_cfg.gpioSimpleEn[1]  = 0;
    io_cfg.gpioComplexEn[0] = 0;
    io_cfg.gpioComplexEn[1] = 0;
    status = CyU3PDeviceConfigureIOMatrix (&io_cfg);
    if (status != CY_U3P_SUCCESS)
    {
        goto handle_fatal_error;
    }

    /* This is a non returnable call for initializing the RTOS kernel */
    CyU3PKernelEntry ();

    /* Dummy return to make the compiler happy */
    return 0;

handle_fatal_error:
    /* Cannot recover from this error. */
    while (1);
}

/* [ ] */

