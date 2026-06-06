#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <malloc.h>
#include <ctype.h>
#include <conio.h>
#include "USBDDOS/CLASS/hid.h"
#include "USBDDOS/DPMI/dpmi.h"
#include "USBDDOS/pic.h"
#include "USBDDOS/dbgutil.h"

//usb keycodes to scan codes and ascii
//ref: USB HID to PS/2 Scan Code Translation Table
//https://download.microsoft.com/download/1/6/1/161ba512-40e2-4cc9-843a-923143f3456c/translate.pdf
static uint8_t USB_HID_KEYBOARD_USAGE2SCANCODES[256*2] = 
{
    0, 0,           //reserved
    0, 0,           //RollOver
    0, 0,           //POSTFail
    0, 0,           //Unddefined
    0x1E, 0,
    0x30, 0,
    0x2E, 0,
    0x20, 0,
    0x12, 0,
    0x21, 0,
    0x22, 0,
    0x23, 0,
    0x17, 0,
    0x24, 0,
    0x25, 0,
    0x26, 0,
    0x32, 0,
    0x31, 0,
    0x18, 0,
    0x19, 0,
    0x10, 0,
    0x13, 0,
    0x1F, 0,
    0x14, 0,
    0x16, 0,
    0x2F, 0,
    0x11, 0,
    0x2D, 0,
    0x15, 0,
    0x2C, 0,
    0x02, 0,
    0x03, 0,
    0x04, 0,
    0x05, 0,
    0x06, 0,
    0x07, 0,
    0x08, 0,
    0x09, 0,
    0x0A, 0,
    0x0B, 0,
    0x1C, 0,   //ENTER
    0x01, 0,    //ESC
    0x0E, 0,   //BACKSPACE
    0x0F, 0,    //TAB
    0x39, 0,    //SPACE
    0x0C, 0,
    0x0D, 0,
    0x1A, 0,
    0x1B, 0,
    0x2B, 0,
    0x2B, 0,    //EUROPE 1 (NOTE2)
    0x27, 0,
    0x28, 0,
    0x29, 0,
    0x33, 0,
    0x34, 0,
    0x35, 0,
    0x3A, 0,    //CAPSLOCK
    0x3B, 0,    //F1
    0x3C, 0,    //F2
    0x3D, 0,    //F3
    0x3E, 0,    //F4
    0x3F, 0,    //F5
    0x40, 0,    //F6
    0x41, 0,    //F7
    0x42, 0,    //F8
    0x43, 0,    //F9
    0x44, 0,    //F10
    0x57, 0,    //F11
    0x58, 0,    //F12

    0x37, 0,    //PrScrn, E0 37
    0x46, 0,    //SCROLLLOCK
    0x1D, 0xE1,   //PAUSE, E1 1D 45
    0x52, 0xE0,    //INSERT
    0x47, 0xE0,    //HOME
    0x49, 0xE0,    //PGUP
    0x53, 0xE0,    //DEL
    0x4F, 0xE0,    //END
    0x51, 0xE0,    //PGDOWN
    0x4D, 0xE0,    //RIGHT ARROW
    0x4B, 0xE0,    //LEFT ARROW
    0x50, 0xE0,    //DOWN ARROW
    0x48, 0xE0,    //UP ARROW

    0x45, 0,    //NUMLOCK
    0x35, 0xE0, //KEYPAD/, E0 35
    0x37, 0,    //KEYPAD*
    0x4A, 0,    //KEYPAD-
    0x4E, 0,    //KEYPAD+
    0x1C, 0xE0, //KEYPAD ENTER, E0 1C
    0x4F, 0,    //KEYPAD1
    0x50, 0,    //KEYPAD2
    0x51, 0,    //KEYPAD3
    0x4B, 0,    //KEYPAD4
    0x4C, 0,    //KEYPAD5
    0x4D, 0,    //KEYPAD6
    0x47, 0,    //KEYPAD7
    0x48, 0,    //KEYPAD8
    0x49, 0,    //KEYPAD9
    0x52, 0,    //KEYPAD0
    0x53, 0,    //KEYPAD.
    0x56, 0,    //EUROPE 2 (NOTE 2)
    0x5D, 0xE0, //APP, E0 5D
    0x5E, 0xE0, //KB POWER, E0 5E
    0x59, 0,    //KEYPAD=
};

//TODO: INT 16h, ah=03h, al=06h: typematic rate and delay //http://mirror.cs.msu.ru/oldlinux.org/Linux.old/docs/interrupts/int-html/rb-1757.htm
#define USB_HID_KEYBOARD_REPEAT_DELAY   300     //300ms
#define USB_HID_KEYBOARD_REPEAT         50      //repeat interval

#define USB_HID_BIOS_SCRLLOCK_S 0x0010 //scroll locked staus
#define USB_HID_BIOS_NUMLOCK_S  0x0020 //num locked staus
#define USB_HID_BIOS_CAPSLOCK_S 0x0040 //caps locked staus
#define USB_HID_BIOS_MMASK      0x0070

/* The 8042 spin-waits are bounded so a non-responsive controller cannot hang
 * the machine - e.g. an emulator that doesn't implement the 0xD3 mouse
 * output-buffer command (OBF never sets), or the absence of any IRQ12 consumer
 * to drain OBF. KBD_8042_SPIN_MAX is ~1s worth of ISA port reads on a 486-class
 * machine, far longer than the microsecond-scale normal 8042 response, so it
 * never trips on working hardware. On timeout the wait breaks and (DEBUG only)
 * logs which wait gave up. */
#define KBD_8042_SPIN_MAX 20000UL
/* Bridge health is judged at FINALIZER ENTRY, not inside the inject. An
 * injected aux byte is only ever consumed after this ISR returns (the IRQ12
 * reflection to the real-mode handler does not run during the in-ISR STI
 * windows on a DPMI-over-V86 stack), so an in-ISR OUT_EMPTY wait expiring is
 * the NORMAL outcome and distinguishes nothing. What does distinguish a live
 * consumer from a dead channel is the state of the output buffer across the
 * inter-report gap (~8-10ms), where consumption IS possible: if the previous
 * report's aux byte (status 0x21: OBF set + aux-data flag) is still parked in
 * the output buffer when the NEXT report arrives, nothing is reading the
 * channel. After MOUSE_STUCK_ENTRIES_SUSPEND consecutive stuck entries the
 * bridge restores the controller (drain stuck aux bytes, re-enable keyboard)
 * and SUSPENDS: reports are then skipped at entry cost, with one real inject
 * retried every MOUSE_INJECT_RETRY_INTERVAL reports; if that retry packet is
 * gone by the following entry, a consumer has appeared and the bridge resumes.
 * The per-byte OUT_EMPTY wait inside the inject is reduced to a short pacing
 * bound (it cannot succeed in-ISR; see above) so a report never burns the full
 * health-scale spin from interrupt context. */
static volatile BOOL g_8042_timeout_hit = FALSE;        //set whenever a full-bound 8042 wait below expires (diagnostic)
static volatile BOOL g_mouse_inject_suspended = FALSE;  //set when entry checks proved nothing consumes the aux channel
static volatile BOOL g_mouse_retry_pending = FALSE;     //a retry report was injected; next entry decides resume vs stay
static volatile uint8_t g_mouse_stuck_entries = 0;      //consecutive entries that found the previous aux byte unconsumed
static volatile uint16_t g_mouse_inject_skip = 0;       //reports skipped while suspended, for periodic retry
#define MOUSE_INJECT_RETRY_INTERVAL 32                  //attempt a real inject every Nth report while suspended
#define MOUSE_STUCK_ENTRIES_SUSPEND 3                   //consecutive stuck entries before the bridge suspends

/* INT 15h/C2xx PS/2 pointing-device emulation
 *
 * The 8042 fake-input bridge above can only ride alongside a real PS/2 mouse:
 * it injects data bytes but answers no device commands, so an INT15-based
 * mouse driver (CuteMouse) probing the aux port finds no device when none is
 * physically present, and an IRQ12-driven consumer can never be fed from the
 * USB ISR anyway (the nested IRQ12 must be deferred for mode-switch safety,
 * and being edge-triggered it is then lost - see DPMI_HWIRQHandlerInternal).
 *
 * Emulating the BIOS INT 15h AX=C2xx pointing-device services removes the
 * 8042 and IRQ12 from the path entirely: the mouse driver registers a far
 * handler via AX=C207h and we call that handler directly with each USB
 * report. Per RBIL (and verified against the CuteMouse 2.1 source), the
 * handler is far-called with four words pushed - status, X, Y, 0 - where
 * X/Y are the raw packet bytes zero-extended (sign carried in the status
 * byte), the handler far-returns without popping, and the caller cleans up.
 *
 * The far call runs through a tiny real-mode thunk. It cannot live in this
 * image's data segment: the 16-bit build keeps zero conventional paragraphs
 * at TSR and runs from its himem copy, so the thunk is placed in a DOS
 * memory block (owned by our PSP, survives TSR) and patched per call via
 * linear stores. Calling down to the registered V86 handler from the USB
 * ISR uses the same DPMI_CallRealModeIRET machinery as every reflected
 * hardware interrupt, and generates no hardware IRQ of its own. */
static DPMI_REG          HID_INT15Reg;                 //RMCB-captured caller registers
static uint32_t          HID_INT15_OldVec = 0;         //previous IVT[15h], chained for non-C2 calls
static volatile uint32_t g_ps2emu_handler = 0;         //C207 handler (seg<<16|off), 0 = none
static volatile uint8_t  g_ps2emu_enabled = 0;         //C200 device-enable state
static uint32_t          HID_PS2ThunkLinear = 0;       //linear addr of the RM call thunk (0 = emu not installed)
static uint16_t          HID_PS2ThunkSeg = 0;

//thunk byte layout (21 bytes), immediates patched at delivery / C207:
//  +0  68 ss ss        push status
//  +3  68 xx xx        push X (zero-extended packet byte)
//  +6  68 yy yy        push Y (zero-extended packet byte)
//  +9  68 00 00        push 0
//  +12 9A oo oo ss ss  call far handler
//  +17 83 C4 08        add sp,8
//  +20 CF              iret  (invoked via DPMI_CallRealModeIRET)
static const uint8_t HID_PS2ThunkTemplate[21] = {
    0x68,0,0, 0x68,0,0, 0x68,0,0, 0x68,0,0,
    0x9A,0,0,0,0, 0x83,0xC4,0x08, 0xCF };

static void USB_HID_INT15Handler(void)
{
    DPMI_REG* r = &HID_INT15Reg;
    if(r->h.ah == 0xC2)
    {
        uint8_t err = 0; //00522 status: 0=ok,1=invalid function,2=invalid input
        switch(r->h.al)
        {
        case 0x00: //enable/disable, BH=state
            if(r->h.bh > 1) { err = 2; break; }
            g_ps2emu_enabled = r->h.bh;
            break;
        case 0x01: //reset: returns BH=device ID, BL=AAh; device left disabled
            g_ps2emu_enabled = 0;
            r->h.bh = 0x00; //standard mouse
            r->h.bl = 0xAA;
            break;
        case 0x02: err = (uint8_t)(r->h.bh > 6 ? 2 : 0); break; //sample rate index
        case 0x03: err = (uint8_t)(r->h.bh > 3 ? 2 : 0); break; //resolution
        case 0x04: r->h.bh = 0x00; break;                       //get type: standard mouse
        case 0x05: //initialize, BH=data package size. Only the plain 3-byte
                   //protocol is emulated; wheel-probe sizes fail with
                   //"invalid input" so the driver falls back to 3-byte mode.
            if(r->h.bh != 3) { err = 2; break; }
            g_ps2emu_enabled = 0;
            break;
        case 0x06: //extended: 0=status, 1/2=scaling
            if(r->h.bh == 0) { r->h.bl = 0x00; r->h.cl = 0x02; r->h.dl = 100; }
            else if(r->h.bh > 2) err = 2;
            break;
        case 0x07: //set device handler, ES:BX (0:0 cancels)
            g_ps2emu_handler = ((uint32_t)r->w.es << 16) | r->w.bx;
            if(HID_PS2ThunkLinear)
            {
                DPMI_StoreW(HID_PS2ThunkLinear + 13, r->w.bx);
                DPMI_StoreW(HID_PS2ThunkLinear + 15, r->w.es);
            }
            break;
        default:   //C208/C209 raw pointer-port access: not emulated
            err = 1;
            break;
        }
        r->h.ah = err;
        if(err) r->w.flags = (uint16_t)(r->w.flags | 1u);  //CF set
        else    r->w.flags = (uint16_t)(r->w.flags & ~1u); //CF clear
        return; //RMCB IRETs back to the caller with these registers
    }

    //not a pointing-device call: chain to the previous INT 15h handler and
    //hand its results back to the caller.
    {
        DPMI_REG r2 = *r;
        r2.w.cs = (uint16_t)(HID_INT15_OldVec >> 16);
        r2.w.ip = (uint16_t)(HID_INT15_OldVec & 0xFFFF);
        r2.w.ss = r2.w.sp = 0;
        DPMI_CallRealModeIRET(&r2);
        r->d.eax = r2.d.eax; r->d.ebx = r2.d.ebx;
        r->d.ecx = r2.d.ecx; r->d.edx = r2.d.edx;
        r->d.esi = r2.d.esi; r->d.edi = r2.d.edi;
        r->d.ebp = r2.d.ebp;
        r->w.es = r2.w.es; r->w.ds = r2.w.ds;
        r->w.fs = r2.w.fs; r->w.gs = r2.w.gs;
        r->w.flags = r2.w.flags;
    }
}

static BOOL USB_HID_PS2Emu_Install(void)
{
    if(HID_PS2ThunkLinear) //already installed
        return TRUE;

    //DOS block for the RM thunk: owned by our PSP, survives TSR (the program
    //image's own conventional memory does not - keep size is 0).
    DPMI_REG r = {0};
    r.h.ah = 0x48;
    r.w.bx = 2; //2 paragraphs
    DPMI_CallRealModeINT(0x21, &r);
    if(r.w.flags & 1u)
    {
        _LOG("PS2EMU: DOS alloc failed\n");
        return FALSE;
    }
    HID_PS2ThunkSeg = r.w.ax;
    HID_PS2ThunkLinear = ((uint32_t)HID_PS2ThunkSeg) << 4;
    for(int i = 0; i < (int)sizeof(HID_PS2ThunkTemplate); ++i)
        DPMI_StoreB(HID_PS2ThunkLinear + (uint32_t)i, HID_PS2ThunkTemplate[i]);

    uint32_t rmcb = DPMI_AllocateRMCB_IRET(&USB_HID_INT15Handler, &HID_INT15Reg);
    if(rmcb == 0)
    {
        _LOG("PS2EMU: RMCB alloc failed\n");
        HID_PS2ThunkLinear = 0;
        return FALSE;
    }

    CLI();
    HID_INT15_OldVec = DPMI_LoadD(0x15ul * 4);
    DPMI_StoreD(0x15ul * 4, rmcb);
    //BIOS equipment word @0040:0010 bit 2: PS/2 pointing device installed.
    //INT15-based drivers check this before probing.
    DPMI_StoreW(0x410, (uint16_t)(DPMI_LoadW(0x410) | 0x0004));
    STI();
    _LOG("PS2EMU: INT15h C2xx pointing-device emulation installed\n");
    return TRUE;
}

#define KBD_8042_PACING_SPIN 256UL                      //token pacing for the in-ISR per-byte OUT_EMPTY (expected to expire)
#if _LOG_ENABLE
static void DBG_8042Timeout(const char* which)
{
    static unsigned long n = 0;
    if(n < 3)
        _LOG("8042 timeout: %s\n", which);
    else if(n == 3)
        _LOG("8042 timeout: further messages suppressed\n");
    ++n;
}
#define _8042_TIMEOUT(s) DBG_8042Timeout(s)
#else
#define _8042_TIMEOUT(s)
#endif
#define WAIT_KEYBOARD_IN_EMPTY() do{ unsigned long _kt=KBD_8042_SPIN_MAX; while((inp(0x64)&2)){ if(!--_kt){ g_8042_timeout_hit = TRUE; _8042_TIMEOUT("IN_EMPTY"); break; } } }while(0)
#define WAIT_KEYBOARD_OUT_EMPTY() do{ unsigned long _kt=KBD_8042_SPIN_MAX; while((inp(0x64)&1)){ STI();NOP();NOP();NOP();CLI(); if(!--_kt){ g_8042_timeout_hit = TRUE; _8042_TIMEOUT("OUT_EMPTY"); break; } } }while(0)//USB_IdleWait()
#define WAIT_EKYBOARD_OUT_FULL() do{ unsigned long _kt=KBD_8042_SPIN_MAX; while(!(inp(0x64)&1)){ if(!--_kt){ g_8042_timeout_hit = TRUE; _8042_TIMEOUT("OUT_FULL"); break; } } }while(0)
//token pacing only: in-ISR consumption never happens (see design note above), so this is expected to expire and neither flags nor logs
#define WAIT_MOUSE_OUT_PACING() do{ unsigned long _kt=KBD_8042_PACING_SPIN; while((inp(0x64)&1)){ STI();NOP();NOP();NOP();CLI(); if(!--_kt) break; } }while(0)

//keyboard device input processing
static BOOL USB_HID_Keyboard_IsInputEmpty(const USB_HID_Data* data);
static int USB_HID_Keyboard_GetInputCount(const USB_HID_Data* data);
static int USB_HID_Keyboard_CompareInput(const USB_HID_Data* data1, const USB_HID_Data* data2);
static int USB_HID_Keyboard_FindNewInput(const USB_HID_Data* data1, const USB_HID_Data* data2); //find new input in data1, return -1 if not found
static BOOL USB_HID_Keyboard_SetupLED(USB_Device* pDevice); //updae LED and BIOS modifier
//keyboard input record management
static BOOL USB_HID_Keyboard_PushRecord(USB_HID_Interface* keyboard, uint8_t KeyIndex);
static uint8_t USB_HID_Keyboard_TopRecord(const USB_HID_Interface* keyboard);
static BOOL USB_HID_Keyboard_RemoveRecord(USB_HID_Interface* keyboard, uint8_t KeyIndex);

static void USB_HID_Keyboard_GenerateKey(uint8_t scancode);
static void USB_HID_Keyboard_Finalizer(void* data);
static void USB_HID_Mouse_GenerateSample(uint8_t byte);
static void USB_HID_Mouse_Finalizer(void* data);
//driver routines
static void USB_HID_DummyCallback(HCD_Request* pRequest) {unused(pRequest);}
static void USB_HID_InputCallback(HCD_Request* pRequest);
static void USB_HID_InputKeyboard(USB_Device* pDevice);
static void USB_HID_InputMouse(USB_Device* pDevice);

BOOL USB_HID_InitDevice(USB_Device* pDevice)
{
    uint8_t bNumInterfaces = pDevice->pConfigList[pDevice->bCurrentConfig].bNumInterfaces;
    USB_InterfaceDesc* pIntfaceDesc0 = pDevice->pConfigList[pDevice->bCurrentConfig].pInterfaces;

    uint16_t DescLength = pDevice->pConfigList[pDevice->bCurrentConfig].wTotalLength;
    uint8_t* pDescBuffer = (uint8_t*)DPMI_DMAMalloc(DescLength, 4);
    uint8_t result = USB_GetConfigDescriptor(pDevice, pDescBuffer, DescLength);
    if(result)
    {
        DPMI_DMAFree(pDescBuffer);
        return FALSE;
    }
    
    USB_HID_DriverData* pDriverData = (USB_HID_DriverData*)malloc(sizeof(USB_HID_DriverData));
    memset(pDriverData, 0, sizeof(USB_HID_DriverData));

    int valid = 0;
    for(int j = 0; j < bNumInterfaces; ++j)
    {
        USB_InterfaceDesc* pIntfaceDesc = pIntfaceDesc0 + j;
        _LOG("HID bInterfaceProtocol: %x\n", pIntfaceDesc->bInterfaceProtocol);
        _LOG("HID bSubClass: %x\n", pIntfaceDesc->bInterfaceSubClass);
        if(pIntfaceDesc->bInterfaceClass == USBC_HID
            && pIntfaceDesc->bInterfaceSubClass == USB_HIDSC_BOOT_INTERFACE
            && (pIntfaceDesc->bInterfaceProtocol == USB_HIDP_KEYBOARD || pIntfaceDesc->bInterfaceProtocol == USB_HIDP_MOUSE) )
        {
            int index = pIntfaceDesc->bInterfaceProtocol - 1;
            assert(index >= USB_HID_KEYBOARD && index <= USB_HID_MOUSE);
            USB_HID_Interface* DrvIntface = &pDriverData->Interface[index];
            DrvIntface->bInterface = pIntfaceDesc->bInterfaceNumber;
            
            for(int i = 0; i < pIntfaceDesc->bNumEndpoints; ++i)
            {
                USB_EndpointDesc* pEndpointDesc = pIntfaceDesc->pEndpoints + i;
                assert(pEndpointDesc->bmAttributesBits.TransferType == USB_ENDPOINT_TRANSFER_TYPE_INTR); //interrupt only
                DrvIntface->pDataEP[pEndpointDesc->bEndpointAddressBits.Dir] = USB_FindEndpoint(pDevice, pEndpointDesc);
                DrvIntface->bEPAddr[pEndpointDesc->bEndpointAddressBits.Dir] = pEndpointDesc->bEndpointAddress;
                if(pEndpointDesc->bEndpointAddressBits.Dir)
                    DrvIntface->Interval = pEndpointDesc->bInterval&0x7FU;
            }
            //_LOG("%x %x\n", DrvIntface->pDataEP[0], DrvIntface->pDataEP[1]);

            uint8_t* desc = pDescBuffer + pIntfaceDesc->offset + pIntfaceDesc->bLength;
            while(*(desc+1) != USB_DT_INTERFACE && desc + *desc < pDescBuffer + DescLength) //until next interface or to the end
            {
                if(*(desc+1) == USB_DT_HID)
                {
                    uint8_t length = *desc;
                    assert(DrvIntface->Descriptors == NULL);
                    DrvIntface->Descriptors = (USB_HID_DESC*)malloc(length); //variable length, need malloc
                    memcpy(DrvIntface->Descriptors, desc, length);
                }
                desc += *desc;
            }

            //set boot protocol
            _LOG("HID: Set boot protocol\n");
            USB_Request req = {USB_REQ_WRITE | USB_REQ_TYPE_HID, USB_REQ_HID_SET_PROTOCOL, USB_HID_PROTOCOL_BOOT, 0, 0};
            req.wIndex = (uint16_t)DrvIntface->bInterface;
            if(DrvIntface->pDataEP[HCD_TXR] != NULL && USB_SyncSendRequest(pDevice, &req, NULL) == 0)
                ++valid;

            //by default the device will send data even if no input data change, set_idle will make it only sending data only changes, i.e. keydown/keyup
            //the original code toggled between SET_IDLE(INDEFINITE) and SET_IDLE(MINIMAL) from
            //USB_HID_InputKeyboard's interrupt-context callback (via async USB_SendRequest) to enable
            //typematic repeat only when keys were held.  That async-from-IRQ dispatch violated
            //EHCI_ControlTransfer's "no pending transfers" assumption (Gap-K, May 2026): a second async
            //SET_IDLE arriving before the first completed could leave the control-pipe QH's Tail.Prev
            //non-NULL, tripping an assertion in EHCI_ControlTransfer.  Workaround A: set MINIMAL idle
            //rate once at install time and keep the device in that state permanently.  Reports come on
            //every polling cycle whether keys are held or not.  Bandwidth cost is ~1 KB/sec per HID
            //device on 12 Mbps USB (under 0.1%), negligible.  Removes the toggle entirely; the
            //DrvIntface->Idle field becomes vestigial.
            _LOG("HID Set idle\n");
            USB_Request req2 = {USB_REQ_WRITE | USB_REQ_TYPE_HID, USB_REQ_HID_SET_IDLE, USB_HID_MAKE_IDLE(1L, USB_HID_IDLE_REPORTALL) /*minimal idle, report every polling cycle - permanent*/, 0, 0};
            req2.wIndex = (uint16_t)DrvIntface->bInterface;
            USB_SyncSendRequest(pDevice, &req2, NULL);
            DrvIntface->Idle = FALSE; /*matches the SET_IDLE(1) state we just programmed*/
        }
    }
    DPMI_DMAFree(pDescBuffer);

    #if DEBUG && 0
    if(pDriverData->Interface[0].Descriptors)
        DBG_DumpB((uint8_t*)pDriverData->Interface[0].Descriptors, pDriverData->Interface[0].Descriptors->bLength, NULL);
    if(pDriverData->Interface[1].Descriptors)
        DBG_DumpB((uint8_t*)pDriverData->Interface[1].Descriptors, pDriverData->Interface[1].Descriptors->bLength, NULL);
    #endif
    if(valid > 0)
        pDevice->pDriverData = pDriverData;
    else
    {
        _LOG("HID: InitDevice failed\n");
        free(pDriverData);
    }
    return valid > 0;
}

BOOL USB_HID_DOS_Install()
{
    int count = 0;
    for(int j = 0; j < USBT.HC_Count; ++j)
    {
        HCD_Interface* pHCI = USBT.HC_List+j;

        for(uint8_t i = 0; i < pHCI->bDevCount; ++i)
        {
            USB_Device* pDevice = HC2USB(pHCI->DeviceList[i]);
            if(pDevice->Desc.bDeviceClass == USBC_HID && pDevice->bStatus == DS_Ready)
            {
                ++count;
                USB_HID_DriverData* pDriverData = (USB_HID_DriverData*)pDevice->pDriverData;

                if(pDriverData->Interface[USB_HID_KEYBOARD].Descriptors != NULL)
                    USB_HID_Keyboard_SetupLED(pDevice); //initial setup of LED. not working for tested keyboard, still dunno why

                _LOG("HID: start driver.\n");
                //start input, aynsc (interrupt)
                for(int i = 0; i < 2; ++i)
                {
                    if(pDriverData->Interface[i].Descriptors && pDriverData->Interface[i].pDataEP[HCD_TXR])
                    {
                        printf("Found USB %s: %s %s\n", i == USB_HID_KEYBOARD ? "keyboard" : "mouse", pDevice->sManufacture, pDevice->sProduct);
                        if(i == USB_HID_MOUSE)
                            USB_HID_PS2Emu_Install(); //once; idempotent
                        USB_Transfer(pDevice, pDriverData->Interface[i].pDataEP[HCD_TXR], pDriverData->Interface[i].Data[0].Buffer, sizeof(USB_HID_Data), &USB_HID_InputCallback, (void*)i);
                    }
                }
            }
        }
    }
    return count > 0;
}

BOOL USB_HID_DOS_Uninstall()
{
    return FALSE;
}

BOOL USB_HID_DeinitDevice(USB_Device* pDevice)
{
    USB_HID_DriverData* pDriverData = (USB_HID_DriverData*)pDevice->pDriverData;
    if(pDriverData)
    {
        if(pDriverData->Interface[USB_HID_KEYBOARD].Descriptors)
            free(pDriverData->Interface[USB_HID_KEYBOARD].Descriptors);
        if(pDriverData->Interface[USB_HID_MOUSE].Descriptors)
            free(pDriverData->Interface[USB_HID_MOUSE].Descriptors);
        free(pDriverData);
    }
    pDevice->pDriverData = NULL;
    return FALSE;
}

void USB_HID_PreInit()
{
    USB_HID_KEYBOARD_USAGE2SCANCODES[0x9A*2] = 0;  //sys req

    USB_HID_KEYBOARD_USAGE2SCANCODES[0xE0*2] = 0x1D;  //left control
    USB_HID_KEYBOARD_USAGE2SCANCODES[0xE1*2] = 0x2A;  //left shift
    USB_HID_KEYBOARD_USAGE2SCANCODES[0xE2*2] = 0x38;  //left alt
    USB_HID_KEYBOARD_USAGE2SCANCODES[0xE3*2] = 0x5B;  //left gui, E0 5B
    USB_HID_KEYBOARD_USAGE2SCANCODES[0xE3*2+1] = 0xE0;
    USB_HID_KEYBOARD_USAGE2SCANCODES[0xE4*2] = 0x1D;  //right control, E0 1D
    USB_HID_KEYBOARD_USAGE2SCANCODES[0xE4*2+1] = 0xE0;
    USB_HID_KEYBOARD_USAGE2SCANCODES[0xE5*2] = 0x36;  //right shift
    USB_HID_KEYBOARD_USAGE2SCANCODES[0xE6*2] = 0x38;  //right alt
    USB_HID_KEYBOARD_USAGE2SCANCODES[0xE6*2+1] = 0xE0;  //right alt
    USB_HID_KEYBOARD_USAGE2SCANCODES[0xE7*2] = 0x5C;  //right gui
    USB_HID_KEYBOARD_USAGE2SCANCODES[0xE7*2+1] = 0xE0;  //right gui
}

void USB_HID_PostDeInit()
{

}

static BOOL USB_HID_Keyboard_IsInputEmpty(const USB_HID_Data* data)
{
    //static char empty[8] = {0};
    //return /*data->Key.modifier == 0 && */memcmp(&data->Key.Keycodes[0], &empty[0], 6) == 0; //treat pure modifier keys are empty, so no repeating for them
    const uint32_t* p = (const uint32_t*)&data->Key;
    return (p[0]&0xFFFF0000) == 0 && p[1] == 0;
}

static int USB_HID_Keyboard_GetInputCount(const USB_HID_Data* data)
{
    int count = 0;
    for(int i = 0; i < 6; ++i)
        count += (data->Key.Keycodes[i] != 0) ? 1 : 0;
    return count;
}

static int USB_HID_Keyboard_CompareInput(const USB_HID_Data* data1, const USB_HID_Data* data2)
{
    int val = (int)data1->Key.Modifier - (int)data2->Key.Modifier;
    //return val != 0 ? val : memcmp(&data1->Key.Keycodes[0], &data2->Key.Keycodes[0], sizeof(data1->Key.Keycodes));
    if(val != 0)
        return val;
    USB_HID_Data d = *data2;
    for(int i = 0; i < 6; ++i)
    {
        uint8_t input = data1->Key.Keycodes[i];
        if(input == 0)
            continue;
        int found;
        for(int j = 0; j < 6; ++j)
        {
            if((found=(input == d.Key.Keycodes[j])))
            {
                d.Key.Keycodes[j] = 0;
                break;
            }
        }
        if(!found)
            return 1;
    }
    return USB_HID_Keyboard_IsInputEmpty(&d) ? 0 : -1;
}

static int USB_HID_Keyboard_FindNewInput(const USB_HID_Data* data1, const USB_HID_Data* data2)
{
    for(int i = 0; i < 6; ++i)
    {
        uint8_t input = data1->Key.Keycodes[i];
        if(input == 0)
            continue;
        int found;
        for(int j = 0; j < 6; ++j)
        {
            if((found=(input == data2->Key.Keycodes[j])))
                break;
        }
        if(!found)
            return input;
    }
    return -1;
}

static BOOL USB_HID_Keyboard_SetupLED(USB_Device* pDevice)
{
    if(pDevice == NULL)
        return FALSE;
    USB_HID_DriverData* pDriverData = (USB_HID_DriverData*)pDevice->pDriverData;
    if(pDriverData == NULL)
        return FALSE;
    uint16_t BiosModifier = DPMI_LoadW(DPMI_SEGOFF2L(0x40,0x17)); //bios data area: 16 bit modifier https://stanislavs.org/helppc/bios_data_area.html
    if(!((BiosModifier^pDriverData->Interface[USB_HID_KEYBOARD].BIOSModifier)&USB_HID_BIOS_MMASK))
        return TRUE;

    int LED = 0;
    if(BiosModifier&USB_HID_BIOS_SCRLLOCK_S) LED |= USB_HID_LED_SCROLLLOCK;
    if(BiosModifier&USB_HID_BIOS_CAPSLOCK_S) LED |= USB_HID_LED_CAPSLOCK;
    if(BiosModifier&USB_HID_BIOS_NUMLOCK_S) LED |= USB_HID_LED_NUMLOCK;
    pDriverData->Interface[USB_HID_KEYBOARD].BIOSModifier = BiosModifier;

    USB_Request req = {USB_REQ_WRITE | USB_REQ_TYPE_HID, USB_REQ_HID_SET_REPORT, USB_HID_MAKE_REPORT(USB_HID_REPORT_OUTPUT, 0), 0, 1}; //1 byte data
    req.wIndex = (uint16_t)pDriverData->Interface[USB_HID_KEYBOARD].bInterface;
    USB_SendRequest(pDevice, &req, &LED, USB_HID_DummyCallback, NULL);
    return TRUE;
}


static BOOL USB_HID_Keyboard_PushRecord(USB_HID_Interface* keyboard, uint8_t KeyIndex)
{
    if(keyboard->RecordCount < 6)
    {
        keyboard->Records[keyboard->RecordCount++] = KeyIndex;
        return TRUE;
    }
    return FALSE;
}

static uint8_t USB_HID_Keyboard_TopRecord(const USB_HID_Interface* keyboard)
{
    if(keyboard->RecordCount == 0)
        return 0;
    return keyboard->Records[keyboard->RecordCount-1];
}

static BOOL USB_HID_Keyboard_RemoveRecord(USB_HID_Interface* keyboard, uint8_t KeyIndex)
{
    for(int i = 0; i < keyboard->RecordCount; ++i)
    {
        if(keyboard->Records[i] == KeyIndex)
        {
            for(int j = i; j < keyboard->RecordCount-1; ++j)
                keyboard->Records[i] = keyboard->Records[i+1];
            --keyboard->RecordCount;
            return TRUE;
        }
    }
    return FALSE;
}

static void USB_HID_Keyboard_GenerateKey(uint8_t scancode)
{
    //putting the key to BIOS keyboard buffer works for applications that use BIOS function (int 16h) to access the keyboard
    //but a lot program doesn't do that. instead, they read the keyborad port
    //so here is another method to fake keyboard input using port IO, from another USB driver made years ago:
    //https://bretjohnson.us/
    //other refs:
    //https://wiki.osdev.org/%228042%22_PS/2_Controller

    //note: WAIT_KEYBOARD_OUT_EMPTY() will temporarily enable interrupt and let IRQ1 handled
    //so that the 2nd scancode after prefix can write to the port
    //the code of https://bretjohnson.us/ didn't do that so it cannot send 2 bytes with prefix
    //we don't need to do special fix Numlock for [HOME,UP ARROW] etc.

    //method 1 from https://bretjohnson.us/
#if 1 //!defined(__BC__)
    WAIT_KEYBOARD_OUT_EMPTY();
    WAIT_KEYBOARD_IN_EMPTY();
    outp(0x64, 0xD2);   //write to out buffer (fake input)
    WAIT_KEYBOARD_IN_EMPTY();
    outp(0x60, scancode);
    WAIT_KEYBOARD_IN_EMPTY();
    WAIT_EKYBOARD_OUT_FULL();
    WAIT_KEYBOARD_OUT_EMPTY();
#else
    //method 2. worked if put in finalizer
    PIC_MaskIRQ(1);
    WAIT_KEYBOARD_IN_EMPTY();
    outp(0x64, 0x60);   //write KCCB
    WAIT_KEYBOARD_IN_EMPTY();
    outp(0x60, scancode);
    WAIT_KEYBOARD_IN_EMPTY();
    outp(0x64, 0x20);   //KCCB to port 60h
    WAIT_KEYBOARD_IN_EMPTY();
    outp(0x64, 0x60);   //write KCCB
    WAIT_KEYBOARD_IN_EMPTY();
    outp(0x60, 0x45);   //return normal mode
    DPMI_REG r = {0};
    DPMI_CallRealModeINT(PIC_IRQ2VEC(1), &r); //call kbd irq
    PIC_UnmaskIRQ(1);
#endif
}

static void USB_HID_Keyboard_Finalizer(void* data)
{
    uint32_t bytes = (uint32_t)(uintptr_t)data;
    uint8_t scancode = (uint8_t)bytes;
    uint8_t prefix = (uint8_t)(bytes>>8);

    uint16_t mask = PIC_GetIRQMask();
    PIC_SetIRQMask(PIC_IRQ_UNMASK(0xFFFF,1));

    if(prefix)
        USB_HID_Keyboard_GenerateKey(prefix);
    USB_HID_Keyboard_GenerateKey(scancode);

    if(prefix == 0xE0 && scancode == 0x46)
    { //Ctrl + Break 0xE0 0x46 0xE0 0xC6
        USB_HID_Keyboard_GenerateKey(0xE0);
        USB_HID_Keyboard_GenerateKey(0xC6);
    }

    PIC_SetIRQMask(mask);
}

#if defined(__DJ2__)
inline
#endif
static void USB_HID_Mouse_GenerateSample(uint8_t byte)
{
    WAIT_KEYBOARD_IN_EMPTY();
    outp(0x64, 0xD3);   //write to out buffer (fake input)
    WAIT_KEYBOARD_IN_EMPTY();
    outp(0x60, byte);
    WAIT_KEYBOARD_IN_EMPTY();

    WAIT_EKYBOARD_OUT_FULL(); //!important: wait until data is available, especially for fast CPUs.

    WAIT_MOUSE_OUT_PACING(); //pacing only: the byte is consumed after this ISR returns, never inside it
}

static void USB_HID_Mouse_8042Restore(void)
{   //a stuck aux byte in the output buffer can block keyboard delivery, leaving
    //the console dead. Discard AUX bytes only (a keyboard byte in OBF belongs
    //to IRQ1 and must not be eaten), then re-enable the keyboard port in case
    //an earlier 0xAE was not accepted by a wedged controller.
    unsigned long drain;
    unsigned long settle;
    uint8_t st;
    for(drain = 0; drain < 16; ++drain)
    {
        settle = 64; //allow a queued byte a moment to reach the output buffer
        while(!((st = (uint8_t)inp(0x64))&1) && --settle);
        if(!(st&1))
            break;   //output buffer stayed empty: drained
        if(!(st&0x20))
            break;   //keyboard data, not aux: leave it for IRQ1
        (void)inp(0x60); //discard the stuck aux byte: nothing is reading it
    }
    WAIT_KEYBOARD_IN_EMPTY();
    outp(0x64, 0xAE); //(re-)enable keyboard port
    WAIT_KEYBOARD_IN_EMPTY();
}

void USB_HID_Mouse_Finalizer(void* data)
{
    USB_HID_Data* hiddata = (USB_HID_Data*)data;

    //INT15h/C2xx delivery: if a pointing-device handler is registered with our
    //emulation, far-call it with this report and skip the 8042 entirely - no
    //controller writes, no IRQ12. Status byte and Y-axis conversion are
    //identical to the 8042 path below (PS/2 Y is positive-up, HID DY is
    //positive-down).
    if(g_ps2emu_handler && g_ps2emu_enabled)
    {
        uint8_t st = (uint8_t)(((hiddata->Mouse.Button&0x7) | 0x08)
                   | (hiddata->Mouse.DX < 0 ? 0x10 : 0)
                   | (hiddata->Mouse.DY > 0 ? 0x20 : 0));
        DPMI_REG tr = {0};
        DPMI_StoreW(HID_PS2ThunkLinear + 1, (uint16_t)st);
        DPMI_StoreW(HID_PS2ThunkLinear + 4, (uint16_t)(uint8_t)hiddata->Mouse.DX);
        DPMI_StoreW(HID_PS2ThunkLinear + 7, (uint16_t)(uint8_t)(-hiddata->Mouse.DY));
        tr.w.cs = HID_PS2ThunkSeg;
        tr.w.ip = 0;
        DPMI_CallRealModeIRET(&tr);
        return;
    }

    //ENTRY-OBF health check (see design note above the WAIT_ macros): the state
    //of the aux output buffer ACROSS the inter-report gap is the only signal
    //that separates "a consumer drains this channel" from "nothing ever will".
    uint8_t entry_status = (uint8_t)inp(0x64);
    BOOL aux_stuck = ((entry_status & 0x21) == 0x21);

    if(g_mouse_inject_suspended)
    {
        if(g_mouse_retry_pending)
        {   //a retry report was injected last time; the gap has passed - decide
            g_mouse_retry_pending = FALSE;
            if(!aux_stuck)
            {   //the retry packet was consumed: a mouse driver is draining the channel
                g_mouse_inject_suspended = FALSE;
                g_mouse_stuck_entries = 0;
                _LOG("PS/2 mouse bridge resumed\n");
                //fall through and inject this report normally
            }
            else
            {   //retry not consumed: stay suspended, keep the controller sane
                USB_HID_Mouse_8042Restore();
                return;
            }
        }
        else
        {
            if(++g_mouse_inject_skip < MOUSE_INJECT_RETRY_INTERVAL)
                return; //skip cheaply: one port read per report
            g_mouse_inject_skip = 0;
            g_mouse_retry_pending = TRUE;
            //fall through: inject one real report as the retry probe
        }
    }
    else
    {
        if(aux_stuck)
        {   //previous report's byte never consumed across the gap. Do NOT stack
            //another report onto it (a 1-deep 8042 would garble both).
            if(++g_mouse_stuck_entries >= MOUSE_STUCK_ENTRIES_SUSPEND)
            {
                USB_HID_Mouse_8042Restore();
                g_mouse_inject_suspended = TRUE;
                g_mouse_retry_pending = FALSE;
                g_mouse_inject_skip = 0;
                g_mouse_stuck_entries = 0;
                _LOG("8042 mouse inject not consumed: suspending PS/2 mouse bridge\n");
            }
            return;
        }
        g_mouse_stuck_entries = 0;
    }

    //https://wiki.osdev.org/Mouse_Input
    int status = (hiddata->Mouse.Button&0x7) | 0x08;
    status |= hiddata->Mouse.DX < 0 ? 0x10 : 0;
    status |= hiddata->Mouse.DY > 0 ? 0x20 : 0;

    uint16_t mask = PIC_GetIRQMask();

    //note: the 3 bytes must be all sent to mouse irq or all the successive mouse data will be corrupted
    
    //we're putting mouse data to the data port and user may press keyboard at the same time
    //disable keyboard port so that the data are not messed up
    WAIT_KEYBOARD_IN_EMPTY();
    outp(0x64, 0xAD); //disable keyboard port
    WAIT_KEYBOARD_IN_EMPTY();

    WAIT_KEYBOARD_OUT_EMPTY(); //flush output. let kbd irq handle it
    outp(0x64, 0xAD); //in case kbd irq re-enable it?
    WAIT_KEYBOARD_IN_EMPTY();

    PIC_SetIRQMask(PIC_IRQ_UNMASK(0xFFFF,12)); //only enable mouse IRQ (12)
    USB_HID_Mouse_GenerateSample((uint8_t)status);
    USB_HID_Mouse_GenerateSample((uint8_t)hiddata->Mouse.DX);
    USB_HID_Mouse_GenerateSample((uint8_t)(-hiddata->Mouse.DY));
    outp(0x64, 0xAE); //enable keyboard port
    WAIT_KEYBOARD_IN_EMPTY();

    PIC_SetIRQMask(mask);
}

static void USB_HID_InputCallback(HCD_Request* pRequest)
{
    USB_Device* pDevice = HC2USB(pRequest->pDevice);
    USB_HID_DriverData* pDriverData = (USB_HID_DriverData*)pDevice->pDriverData;

    int mode = (int)(uintptr_t)pRequest->pCBData;
    if(mode == USB_HID_KEYBOARD)
        USB_HID_InputKeyboard(pDevice);
    else
        USB_HID_InputMouse(pDevice);

    //continue input, async don't wait
    int index = (pDriverData->Interface[mode].Index+1)&0x1;
    pDriverData->Interface[mode].Index = (uint8_t)index&0x1U;
    USB_Transfer(pDevice, pDriverData->Interface[mode].pDataEP[HCD_TXR], pDriverData->Interface[mode].Data[index].Buffer, sizeof(USB_HID_Data), &USB_HID_InputCallback, (void*)mode);
}

static void USB_HID_InputKeyboard(USB_Device* pDevice)
{
    USB_HID_DriverData* pDriverData = (USB_HID_DriverData*)pDevice->pDriverData;
    USB_HID_Interface* kbd = &pDriverData->Interface[USB_HID_KEYBOARD];
    USB_HID_Data* data = &kbd->Data[kbd->Index];
    USB_HID_Data* prev = &kbd->Data[(kbd->Index+1)&0x1];
    BOOL empty = USB_HID_Keyboard_IsInputEmpty(data);

    //Gap-K Workaround A (May 2026): the device is configured at install time with SET_IDLE(MINIMAL)
    //and stays in that state permanently.  The previous code toggled between SET_IDLE(INDEFINITE)
    //and SET_IDLE(MINIMAL) from this interrupt-context callback via async USB_SendRequest, which
    //violated EHCI_ControlTransfer's "no pending transfers" assumption and could trip an assertion
    //(see HID_DOS_Install for full context).  The kbd->Idle field is kept for ABI compatibility but
    //is no longer read; it stays at the value set during install (FALSE for the minimal-idle config).

    USB_HID_Keyboard_SetupLED(pDevice);
    
    int count = USB_HID_Keyboard_GetInputCount(data);
    int PrevCount = kbd->PrevCount;
    kbd->PrevCount = (uint8_t)count;

    //_LOG("delay timer: %d, rpt timer: %d\n", kbd->DelayTimer, kbd->RepeatingTimer);
    static const int KEYOUT_DOWN = 1;
    static const int KEYOUT_REPEAT = 2;

    int DoOut = 0;
    BOOL InputChanged = (count != PrevCount) || (USB_HID_Keyboard_CompareInput(prev, data) != 0);
    if(InputChanged) //input changed
    {
        kbd->DelayTimer = 0;
        kbd->RepeatingTimer = 0;
        DoOut = KEYOUT_DOWN;
    }
    else
    {
        if(kbd->DelayTimer >= USB_HID_KEYBOARD_REPEAT_DELAY)
        {
            kbd->RepeatingTimer = (uint16_t)(kbd->RepeatingTimer+kbd->Interval);
            if(kbd->RepeatingTimer >= USB_HID_KEYBOARD_REPEAT)
            {
                DoOut = KEYOUT_REPEAT;
                kbd->RepeatingTimer = 0;
            }
        }                
        else
            kbd->DelayTimer = (uint16_t)(kbd->DelayTimer+kbd->Interval);
    }
    if(!DoOut || (!InputChanged && empty))
        return;

    #if 0
    if(InputChanged)
    {
        DBG_DumpB(data->Key.Keycodes, 6, NULL);
        DBG_DumpB(prev->Key.Keycodes, 6, NULL);
    }
    #endif
    int index = InputChanged ?
        (count < PrevCount ? USB_HID_Keyboard_FindNewInput(prev, data) : USB_HID_Keyboard_FindNewInput(data, prev))
        : USB_HID_Keyboard_TopRecord(kbd); //compare to find the change. by the spec, the new downed key is not always the first

    if(index == -1) //only modifier changed. seems that modifier keys won't get to keycodes but only the modifier mask.
    {
        uint8_t modifier = prev->Key.Modifier^data->Key.Modifier;
        if(modifier != 0)
        {
            uint32_t bitIndex = BSF((uint32_t)modifier);
            index = 0xE0 + (int)bitIndex; //modifier key inited in USB_HID_PreInit. the bit order is the same order in the table.
        }
        //else _LOG("ERROR");
        if(prev->Key.Modifier&modifier) //prev exist, now cleared. fake break (PrevCount > count)
            PrevCount = count + 1;
    }

    uint8_t scancode = USB_HID_KEYBOARD_USAGE2SCANCODES[index*2];
    uint8_t prefix = USB_HID_KEYBOARD_USAGE2SCANCODES[index*2+1];
    if(count > PrevCount) //key make
        USB_HID_Keyboard_PushRecord(kbd, (uint8_t)index);
    else if(count < PrevCount) //key break
    {
        USB_HID_Keyboard_RemoveRecord(kbd, (uint8_t)index); //remove key record
        scancode = (uint8_t)(scancode + 0x80);
    }

    //_LOG("%d %02x %02x", index, prefix, scancode);

    if(prefix == 0xE1) //pause/break. handle ctrl+break
    {
        if(scancode == 0x1D+0x80 || !(data->Key.Modifier&(USB_HID_LCTRL|USB_HID_RCTRL)) || DoOut == KEYOUT_REPEAT) //key up, or no ctrl pressed, or repeat
            return;
        prefix = 0xE0;
        scancode = 0x46;
    }
        
    //IRQ1 handler will send EOI and conflict with current interrupt handler.
    //need send our EOI first. add support to custom finalize function to do key out after USB EOI
    USB_ISR_Finalizer* finalizer = (USB_ISR_Finalizer*)malloc(sizeof(USB_ISR_Finalizer));
    finalizer->FinalizeISR = USB_HID_Keyboard_Finalizer;
    finalizer->data = (void*)(uintptr_t)((prefix<<8) | scancode);
    USB_ISR_AddFinalizer(finalizer);
}

static void USB_HID_InputMouse(USB_Device* pDevice)
{
    USB_HID_DriverData* pDriverData = (USB_HID_DriverData*)pDevice->pDriverData;
    USB_HID_Interface* mouse = &pDriverData->Interface[USB_HID_MOUSE];
    USB_HID_Data* data = &mouse->Data[mouse->Index];
    USB_HID_Data* prev = &mouse->Data[(mouse->Index+1)&0x1];

    //Skip idle reports. Gap-K keeps the device in minimal idle (SET_IDLE report-every-cycle),
    //so the device sends a report on EVERY polling cycle whether or not anything moved. Without
    //this guard the 8042 PS/2-injection finalizer below is queued on every cycle and runs the full
    //(slow, multi-port-I/O) injection dance from USB-IRQ context continuously, even with the mouse
    //at rest - starving the DOS foreground so control never returns after the driver goes resident.
    //The keyboard path has the equivalent no-change guard (see USB_HID_InputKeyboard). DX/DY are
    //relative deltas, so zero means no motion; Button is compared against the PREVIOUS report rather
    //than against zero so a button-release (button bit clears) still generates a sample - that was
    //the defect that caused the original guard here to be commented out.
    if(data->Mouse.DX == 0 && data->Mouse.DY == 0 && data->Mouse.Button == prev->Mouse.Button)
        return;

    USB_ISR_Finalizer* finalizer = (USB_ISR_Finalizer*)malloc(sizeof(USB_ISR_Finalizer));
    finalizer->FinalizeISR = USB_HID_Mouse_Finalizer;
    finalizer->data = data;
    USB_ISR_AddFinalizer(finalizer);
}
