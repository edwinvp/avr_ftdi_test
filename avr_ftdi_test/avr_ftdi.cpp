// avr_ftdi.cpp
// Created: 20-9-2019 21:30:13
// Author : E. van Putten (edwinvp@xs4all.nl)
// Derived from program by Michael Davidsaver (see notes).
//
// Title: 
//  Atmel based FTDI USB serial converter emulation.
//
// Description / what it is:
//  This program of EXPERIMENTAL nature allows emulating a specific
//  FTDI USB to serial converter from your Atmel ATMega 32u4 (or compatible)
//  controller.
//
//  It sets up the Atmel USB peripheral to match a FT232BM style device.
//  When running and connected to a pc/laptop running your favorite terminal
//  software, some of the chars typed on the pc/laptop will be echoed back.
//  Typing the 'a' character will print a rather famous message.
//
// What it is NOT:
//  This will not give you a fully working USB to serial converter like the real 
//  FT232BM chip does.
//  No attempt has been made to ensure correct forwarding of bytes across interfaces
//  etc. In fact, there are even characters mixed into the regular USART stream
//  to aid debugging the USB enumeration process!
//  Also note that only a minimal/limited set of the official vendor specific commands are
//  responded to.
// 
// Audience:
//  This all might be useful for someone (like me) who...
//  ... needs to have two independent serial ports from their Atmel controller
//  ... is intimidated by (or doesn't want to use) either Atmel's ASF or LUFA
//  ... wants to tinker with USB and use this as a starting point
//  ... just wants to see a minimal program instead of tons of source files
//
// IMPORTANT NOTES:
// 1. This work is derived, with permission, from Michael Davidsaver's `simpleusb.c` program.
//    His original software can be found here:
//    https://github.com/mdavidsaver/avr/blob/master/simpleusb.c
// 2. Use this software at your own risk! Playing around with low level USB stuff
//    is a notorious way to crash your computer, possibly corrupting some files
//    you have open in the process. Don't say I didn't warn you!
// 3. This code is meant for demonstration/hobby purposes only.
//    It is most probably not a good idea to emulate nor impersonate FTDI's products 
//    in any serious commercial product you might have.
//
// BUGS / LIMITATIONS:
// 1. Only tested on Arduino Leonardo board with 16 MHz crystal/oscillator
// 2. The real FTDI has a EP0 size of 8 bytes, this program uses 64.
//    Makes it easier to program the Atmel that way.
// 3. The character echoed back is only the last character when a bunch of characters
//    is actually received in one transaction. This is to keep the example simple.
//    A more useful program will need some kind of line buffer, FIFO etc.
//    Your turn to experiment!
// 4. Any USB power management / suspend related events/interrupts have not been
//    taken into consideration. Things might break if you surprise remove the device!
// 5. A number of vendor (FTDI) specific commands are acknowledged to keep the 
//    original drivers happy, but are simply ignored.
//    For instance setting the baud rate has no effect at all.
//    Don't expect the regular USART peripheral to change settings based on these.
// 6. FTDI EEPROM reads always give an output of all FF FF hex for the same reason.
// 7. Unlike the original simple usb program, the file has turned half into C++, not C,
//    which might annoy or offend some programmers. Sorry!

#include "settings.h"
#include <avr/io.h>
#include <stdio.h>
#include <stdlib.h>
#include <avr/delay.h>
#include <avr/pgmspace.h>
#include <avr/interrupt.h>
#include "uart.h"
#include "usb.h"

// Variable to store incoming byte received from the regular USART
static unsigned char uart_byte = 0;

static
uint8_t ctrl_write_PM(const void *addr, uint16_t len);

#define set_bit(REG, BIT) REG |= _BV(BIT)
#define clear_bit(REG, BIT) REG &= ~_BV(BIT)
#define toggle_bit(REG, BIT) REG ^= _BV(BIT)
#define assign_bit(REG, BIT, VAL) do{if(VAL) set_bit(REG,BIT) else clear_bit(REG,BIT);}while(0)

// Endpoint 0 size
// NOTE: FTDI defines this as 8 bytes instead, but 64 is much easier to program as we don't have
// split up the bigger transfers.
#define EP0_SIZE 64 

#define EP_select(N) do{UENUM = (N)&0x07;}while(0)
#define EP_read8() (UEDATX)
#define EP_read16_le() ({uint16_t L, H; L=UEDATX; H=UEDATX; (H<<8)|L;})
#define EP_write8(V) do{UEDATX = (V);}while(0)
#define EP_write16_le(V) do{UEDATX=(V)&0xff;UEDATX=((V)>>8)&0xff;}while(0)

	
ISR(WDT_vect)
{
	
}

ISR(USART1_RX_vect)
{
	uart_byte=UDR1;
}

void oops(int a, char * v)
{	
	if (!a) {
		printf_P(PSTR("oops! %s"),v);
		while (1)
			;
	}
}

void put_hex(unsigned int i)
{	
	printf_P(PSTR("%04x"),i);
}


/* The control request currently being processed */

static usb_header head;

/* USB descriptors, stored in flash */
static const usb_std_device_desc PROGMEM devdesc = {
    sizeof(devdesc),
    usb_desc_device,
    0x0110, // 0x0110 for USB v1.1, 0x0200 for USB v2.0
    0x00, /* vendor specific / device class */
    0x00, /* vendor specific / device sub class */
    0x00, /* vendor specific / device protocol */
    EP0_SIZE, /* EP 0 size 64 bytes, real FTID reports 8 */
    0x0403, // Vendor ID (VID): Future Technology Devices International Limited
    0x6001, // Product ID (PID): FT232
    0x0400, // bcdDevice
	1, // iManufacturer
    2, // iProduct
    0, // iSerialNumber (has nothing to do with that alfanumeric FTDI serial number)
    1 // Number of configurations
};

struct tdevconf
{
	usb_std_config_desc conf;
	usb_std_iface_desc iface;
	usb_std_EP_desc ep1;
	usb_std_EP_desc ep2;
};

static const PROGMEM tdevconf devconf {
	
	{
        sizeof(usb_std_config_desc),
        usb_desc_config,
        sizeof(devconf),
        1,
        1,
		0, 
        0x80, // bus powered
        20/2  // 20 mA		
	},	
	{
        sizeof(usb_std_iface_desc),
        usb_desc_iface,
        0,
        0,
        2, // # of endpoints
        0xff, // vender specific
        0xff, // vender specific
        0xff, // vender specific
		0
	},
	// End point 1
	{
		7, // size
		usb_desc_EP,
		0x81, // address
		0x02, // bulk
		0x0040, // max packet size (64 bytes)
		0, // interval		
	},
	// End point 2
	{
		7, // size
		usb_desc_EP,
		0x02, // address
		0x02, // bulk
		0x0040, // max packet size (64 bytes)
		0, // interval
	}
};

static const usb_std_string_desc iLang PROGMEM = {
	4, //sizeof(usb_std_string_desc) + 2,
	usb_desc_string,
    L"\x0409"
};

/*
#define DESCSTR(STR) { \
	usb_desc_string, \
	STR \
}
*/

#define DESCSTR(STR) { \
	2 + sizeof(STR) - 2, \
	usb_desc_string, \
	STR \
}


// USB product name ("friendly name") that shows up when the host is quizzing the device
static const usb_std_string_desc iProd PROGMEM = DESCSTR(L"QuartelRCBB\0\0");

// FTDI style alphanumeric serial number
static const usb_std_string_desc iSerial PROGMEM = DESCSTR(L"FTP1W65N\0\0");

#undef DESCSTR

/* Handle the standard Get Descriptor request.
 * Return 1 on success
 */
static
uint8_t USB_get_desc(void)
{
    const void *addr;
    uint8_t len, idx = head.wValue;
    switch(head.wValue>>8)
    {
    case usb_desc_device:
        if(idx!=0) return 0;
        addr = &devdesc; len = sizeof(devdesc);
        break;
    case usb_desc_config:
        if(idx!=0) return 0;
        addr = &devconf; len = sizeof(devconf);
        break;
    case usb_desc_string:
        switch(idx)
        {
        case 0: addr = &iLang; break;
        case 1: addr = &iProd; break;
        case 2: addr = &iSerial; break;
        default: return 0;
        }
        /* the first byte of any descriptor is its length in bytes */
        len = pgm_read_byte(addr);
        break;
    default:
        return 0;
    }

    if(len>head.wLength)
        len = head.wLength;

    return !ctrl_write_PM(addr, len);
}

static void setupEP0(void);

static uint16_t userval; /* user register */

static inline void setupusb(void)
{
    /* disable USB interrupts and clear any active */
    UDIEN = 0;
    UDINT = 0;

    set_bit(UDCON, DETACH); /* redundant? */

    /* toggle USB reset */
    clear_bit(USBCON, USBE);
    set_bit(USBCON, USBE);

    /* No required.
     * Gives some time to start reprogramming
     * if previous program gets stuck right away
     */
    _delay_ms(1000);
    putchar('.');

    /* Unfreeze */
    clear_bit(USBCON, FRZCLK);

    /* setup PLL for 8 MHz system clock */
    PLLCSR = 0;
    set_bit(PLLCSR, PLLE);
    loop_until_bit_is_set(PLLCSR, PLOCK);
    putchar('.');

    setupEP0(); /* configure control EP */
    putchar('.');

#ifdef HANDLE_SUSPEND
    set_bit(UDIEN, SUSPE);
#endif
    set_bit(UDIEN, EORSTE);

    /* allow host to un-stick us.
     * Warning: Don't use w/ DETACH on CPU start
     *          or a reset loop will result
     */
    //set_bit(UDCON, RSTCPU);
    clear_bit(UDCON, DETACH);
}

/* Setup the control endpoint. (may be called from ISR) */
static void setupEP0(void)
{
    /* EPs assumed to be configured in increasing order */

    EP_select(0);

    /* un-configure EP 0 */
    clear_bit(UECONX, EPEN);
    clear_bit(UECFG1X, ALLOC);

    /* configure EP 0 */
    set_bit(UECONX, EPEN);
    UECFG0X = 0; /* CONTROL */
    UECFG1X = 0b00110010; // EPSIZE=64B, 1 bank, ALLOC 
#if EP0_SIZE!=64
#  error EP0 size mismatch
#endif

    if(bit_is_clear(UESTA0X, CFGOK)) {
        putchar('!');
        while(1) {} /* oops */
    }
}

static void setup_other_ep()
{
	// The FTDI has two endpoints for serial data, they are:
	//
	// Endpoint 1 (IN):
	//   bEndpointAddress:     0x81
	//   Transfer Type:        Bulk
	//   wMaxPacketSize:     0x0040 (64)
	//   bInterval:            0x00
	//
	// Endpoint 2 (OUT):
	//   bEndpointAddress:     0x02
	//   Transfer Type:        Bulk
	//   wMaxPacketSize:     0x0040 (64)
	//   bInterval:            0x00

    EP_select(1);

    // un-configure EP 1
    clear_bit(UECONX, EPEN);
    clear_bit(UECFG1X, ALLOC);

    // configure EP 1
    set_bit(UECONX, EPEN);
    UECFG0X = 0x81; // BULK, IN
    UECFG1X = 0b00110010; // EPSIZE=64B, 1 bank, ALLOC

	if(bit_is_clear(UESTA0X, CFGOK)) {
		putchar('1!');
		while(1) {} /* oops */
	}

    EP_select(2);

    // un-configure EP 2
    clear_bit(UECONX, EPEN);
    clear_bit(UECFG1X, ALLOC);

    // configure EP 2
    set_bit(UECONX, EPEN);
    UECFG0X = 0x80; /* BULK, OUT */
    UECFG1X = 0b00110010; // EPSIZE=64B, 1 bank, ALLOC

    if(bit_is_clear(UESTA0X, CFGOK)) {
	    putchar('1!');
	    while(1) {} /* oops */
    }

    EP_select(0);	
	
}

ISR(USB_GEN_vect, ISR_BLOCK)
{
    uint8_t status = UDINT, ack = 0;
    putchar('I');
#ifdef HANDLE_SUSPEND
    if(bit_is_set(status, SUSPI))
    {
        ack |= _BV(SUSPI);
        /* USB Suspend */

        /* prepare for wakeup */
        clear_bit(UDIEN, SUSPE);
        set_bit(UDIEN, WAKEUPE);

        set_bit(USBCON, FRZCLK); /* freeze */
    }
    if(bit_is_set(status, WAKEUPI))
    {
        ack |= _BV(WAKEUPI);
        /* USB wakeup */
        clear_bit(USBCON, FRZCLK); /* freeze */

        clear_bit(UDIEN, WAKEUPE);
        set_bit(UDIEN, SUSPE);
    }
#endif
    if(bit_is_set(status, EORSTI))
    {
        ack |= _BV(EORSTI);
        /* coming out of USB reset */

#ifdef HANDLE_SUSPEND
        clear_bit(UDIEN, SUSPE);
        set_bit(UDIEN, WAKEUPE);
#endif

        putchar('E');
        setupEP0();
    }
    /* ack. all active interrupts (write 0)
     * (write 1 has no effect)
     */
    UDINT = ~ack;
}

/* function to write bytes of program memory to a bulk endpoint 
BUG/LIMITATION: this function does not allow writing big chunks, because
the bulk endpoints in this program have a max size of 64 bytes */
static
void bulk_write_PM(const void *addr, uint16_t len)
{
    while(len--) {
	    uint8_t val = pgm_read_byte(addr);
		UEDATX = val;
		addr++;
    }	
}


/* write value from flash to EP0 */
static
uint8_t ctrl_write_PM(const void *addr, uint16_t len)
{

    while(len) {
        uint8_t ntx = EP0_SIZE,
                bsize = UEBCLX,
                epintreg = UEINTX;

        oops(ntx>=bsize, "EP"); /* EP0_SIZE is wrong */

        ntx -= bsize;
        if(ntx>len)
            ntx = len;

        if(bit_is_set(epintreg, RXSTPI))
            return 1; /* another SETUP has started, abort this one */
        if(bit_is_set(epintreg, RXOUTI))
            break; /* stop early? (len computed improperly?) */

        /* Retry until can send */
        if(bit_is_clear(epintreg, TXINI))
            continue;
        oops(ntx>0, "Ep"); /* EP0_SIZE is wrong (or logic error?) */

        len -= ntx;

        while(ntx) {
            uint8_t val = pgm_read_byte(addr);
            EP_write8(val);
            addr++;
            ntx--;
        }

        clear_bit(UEINTX, TXINI);
    }
    return 0;
}

/* Handle standard Set Address request */
static
void USB_set_address(void)
{
    UDADDR = head.wValue&0x7f;

    clear_bit(UEINTX, TXINI); /* send 0 length reply */
    loop_until_bit_is_set(UEINTX, TXINI); /* wait until sent */

    UDADDR = _BV(ADDEN) | (head.wValue&0x7f);

    clear_bit(UEINTX, TXINI); /* magic packet? */
}

static
uint8_t USB_config;

static
void USB_set_config(void)
{
	USB_config = head.wValue;
	
	setup_other_ep();
}

// Called when we encounter an 'alien' USB request/message so we can work out what 
// is needed to support it
static void dump_unsupported_request(void)
{
	putchar('?');
	put_hex(head.bmReqType);
	put_hex(head.bReq);
	put_hex(head.wLength>>8);
	put_hex(head.wLength);	
}

// Handles CONTROL reads (Atmel to pc)
static void usb_control_in(void)
{	
	// Flag that indicates whether the request was supported and should be ack'ed.
	// If at the end of the function it is false, then a the endpoint is STALLed
    uint8_t ok = 0; 

    switch(head.bReq)
    {
    case usb_req_set_feature:
    case usb_req_clear_feature:
        /* No features to handle.
         * We ignore Remote wakeup,
         * and EP0 will never be Halted
         */
        ok = 1;
        break;
    case usb_req_get_status:
        switch(head.bmReqType) {
        case USB_REQ_TYPE_IN:
        case USB_REQ_TYPE_IN | USB_REQ_TYPE_INTERFACE:
        case USB_REQ_TYPE_IN | USB_REQ_TYPE_ENDPOINT:
            // always status 0
            loop_until_bit_is_set(UEINTX, TXINI);
            EP_write16_le(0);
            clear_bit(UEINTX, TXINI);
            ok = 1;
        }
        break;
    case usb_req_set_address:
		// This is an 'out' command, so should be handled by 'usb_control_out' instead
        break;
    case usb_req_get_desc:
        if(head.bmReqType==0x80) {
            ok = USB_get_desc();
        }
        break;
    case usb_req_set_config:
        if(head.bmReqType==0) {
            USB_config = head.wValue;
            ok = 1;
        }
        break;
    case usb_req_get_config:
        if(head.bmReqType==USB_REQ_TYPE_IN) {
            loop_until_bit_is_set(UEINTX, TXINI);
            EP_write8(USB_config);
            clear_bit(UEINTX, TXINI);
            ok = 1;
        }
        break;
    case usb_req_set_iface:
		break;
    case usb_req_get_iface:
		break;
    case usb_req_set_desc:
		break;
    case usb_req_synch_frame:
        break;

    default:
		if ((head.bmReqType & USB_REQ_TYPE_VENDOR)==0) {
			dump_unsupported_request();
		}
    }

	// Vendor specific	
	if (head.bmReqType == (USB_REQ_TYPE_IN|USB_REQ_TYPE_VENDOR)) {
		switch (head.bReq) {
		case FTDI_SIO_READ_EEPROM:
			loop_until_bit_is_set(UEINTX, TXINI);
			EP_write8(0xff);
			EP_write8(0xff);
			clear_bit(UEINTX, TXINI);
			ok=1;
		
			break;

		case FTDI_SIO_GET_LATENCY_TIMER:
			// 16 ms is the default value
			loop_until_bit_is_set(UEINTX, TXINI);
			EP_write8(0x10); // 16 [ms] is the default value
			clear_bit(UEINTX, TXINI);
			ok=1;
			break;
		case FTDI_SIO_GET_MODEM_STATUS:
			// 16 ms is the default value
			loop_until_bit_is_set(UEINTX, TXINI);
			EP_write8(0x00); // 16 [ms] is the default value
			clear_bit(UEINTX, TXINI);
			ok=1;
			break;
		default:
			dump_unsupported_request();			
		};	
		
	}
	
    if(ok) {
        if(head.bmReqType&ReqType_DirD2H) {
            /* Control read.
             * Wait for, and complete, status
             */
            uint8_t sts;
            while(!((sts=UEINTX)&(_BV(RXSTPI)|_BV(RXOUTI)))) {}
            //loop_until_bit_is_set(UEINTX, RXOUTI);
            ok = (sts & _BV(RXOUTI));
            if(!ok) {
                set_bit(UECONX, STALLRQ);
                putchar('S');
            } else {
                clear_bit(UEINTX, RXOUTI);
                clear_bit(UEINTX, TXINI);
            }
        } else {
            /* Control write.
             * indicate completion
             */
            clear_bit(UEINTX, TXINI);
        }
        putchar('C');

    } else {
        /* fail un-handled SETUP */
        set_bit(UECONX, STALLRQ);
        putchar('F');
    }
}

// Handles CONTROL writes (pc to Atmel)
static void usb_control_out(void)
{
	uint8_t ok = 0;

    switch(head.bReq)
    {
    case usb_req_set_feature:
    case usb_req_clear_feature:
        /* No features to handle.
         * We ignore Remote wakeup,
         * and EP0 will never be Halted
         */
        ok = 1;
        break;
    case usb_req_get_status:
		// This is an 'in' command, so should be handled by `usb_control_in` instead
        break;
    case usb_req_set_address:
        if(head.bmReqType==USB_REQ_TYPE_OUT) {
			// Host sets USB address
            putchar('A');
            USB_set_address();		
            putchar('a');
			
            return;
        }
        break;
    case usb_req_get_desc:
		// This is an 'in' command, so should be handled by 'usb_control_in' instead
        break;
    case usb_req_set_config:
        if(head.bmReqType==0) {
			putchar('S');
			USB_set_config();
			putchar('s');			
            ok = 1;					
        }
        break;
    case usb_req_get_config:
        break;
    case usb_req_set_iface:
		break;
    case usb_req_get_iface:
		break;
    case usb_req_set_desc:
		break;
    case usb_req_synch_frame:
        break;

    default:
		if ((head.bmReqType & USB_REQ_TYPE_VENDOR)==0) {
			dump_unsupported_request();
		}
    }
	
	// Vendor specific
	if (head.bmReqType == (USB_REQ_TYPE_OUT|USB_REQ_TYPE_VENDOR)) {
		switch (head.bReq) {
		case FTDI_SIO_RESET:
			ok=1;
			break;			
		case FTDI_SIO_MODEM_CTRL:
		case FTDI_SIO_SET_BAUD_RATE:
		case FTDI_SIO_SET_DATA:
		case FTDI_SIO_SET_FLOW_CTRL:
		case FTDI_SIO_SET_LATENCY_TIMER:
			ok=1;
			break;
		default:
			dump_unsupported_request();
		};		
	}

    if(ok) {
        if(head.bmReqType&ReqType_DirD2H) {
            /* Control read.
             * Wait for, and complete, status
             */
            uint8_t sts;
            while(!((sts=UEINTX)&(_BV(RXSTPI)|_BV(RXOUTI)))) {}
            //loop_until_bit_is_set(UEINTX, RXOUTI);
            ok = (sts & _BV(RXOUTI));
            if(!ok) {
                set_bit(UECONX, STALLRQ);
                putchar('S');
            } else {
                clear_bit(UEINTX, RXOUTI);
                clear_bit(UEINTX, TXINI);
            }
        } else {
            /* Control write.
             * indicate completion
             */
            clear_bit(UEINTX, TXINI);
        }
        putchar('C');

    } else {
        /* fail un-handled SETUP */
        set_bit(UECONX, STALLRQ);
        putchar('F');
    }
}

// Whether we need to send Hello World message to the pc
static bool do_send_famous_message = false;
// Whether we need to echo a character to the pc
static bool do_send_char = false;
// Last usb "serial" character received as sent by pc/laptop
static char usb_char = '\0';

// Every FTDI serial read starts with two reserved bytes
void send_reserved_bytes()
{
	// The original device reserves the first two bytes for the modem and line status
	UEDATX = 0x80; // Modem status.
	UEDATX = 0x00; // Line status.	
}

// Possibly send bytes to the pc/laptop
void handle_outgoing_bytes(void)
{
	// Turn attention to the bulk IN endpoint, because that's were bytes
	// destined for the pc/laptop should go to first
	EP_select(1);
	
	if (do_send_famous_message) {
		do_send_famous_message=false;
		
		if (bit_is_set(UEINTX,TXINI)) {
			clear_bit(UEINTX,TXINI);
			send_reserved_bytes();
			// Write the famous message to the pc/laptop
			bulk_write_PM(PSTR("Hello world!\r\n"),14);			
			clear_bit(UEINTX,FIFOCON);
		}		
	} else if (do_send_char) {
		do_send_char=false;

		if (bit_is_set(UEINTX,TXINI)) {
			clear_bit(UEINTX,TXINI);			
			send_reserved_bytes();
			// Send one char
			UEDATX = usb_char;			
			clear_bit(UEINTX,FIFOCON);		
		}
	}
	
}

// Possibly receive bytes from the pc/laptop
void handle_incoming_bytes(void)
{
	// Turn attention to the bulk OUT endpoint, because that's were bytes
	// sent from the pc/laptop end up in.
	EP_select(2);
	
	if (bit_is_set(UEINTX, RXOUTI)) {
		// Acknowledge receive int
		clear_bit(UEINTX, RXOUTI);
			
		// See how much bytes we got
		unsigned int N = ((unsigned int)UEBCHX << 8) | UEBCLX;
		
		if (N>0) {
			// Read chars sent by the pc/laptop
			for (int i(0);i<N;i++) {
				usb_char = UEDATX;
				
				if (usb_char=='a')
					do_send_famous_message=true;
				else
					do_send_char=true;					
			}
		}
		
		clear_bit(UEINTX,FIFOCON);
	}	
}

// Called when the pc/laptop is quizzing/configuring the Atmel
static
void handle_CONTROL(void)
{
    uint8_t ok = 0;
    /* SETUP message */
    head.bmReqType = EP_read8();
    head.bReq = EP_read8();
    head.wValue = EP_read16_le();
    head.wIndex = EP_read16_le();
    head.wLength = EP_read16_le();

    /* ack. first stage of CONTROL.
     * Clears buffer for IN/OUT data
     */
    clear_bit(UEINTX, RXSTPI);

    /* despite what the figure in
     * 21.12.2 (Control Read) would suggest,
     * SW should not clear TXINI here
     * as doing so will send a zero length
     * response.
     */
	
	if (head.bmReqType & USB_REQ_TYPE_IN)
		usb_control_in();
	else
		usb_control_out();	
}

ISR(USB_COM_vect)
{
	// This USB interrupt isn't used.
}

// Performs initial USB and PLL configuration
void setup_usb()
{
	// Start with interrupts disabled
	cli();

	// disable USB general interrupts
	USBCON &= 0b11111110;
	// disable all USB device interrupts
	UDIEN &= 0b10000010;
	// disable USB endpoint interrupts
	UEIENX &= 0b00100000;

	// Re-enable interrupts
	sei();

	// Enable USB pad regulator
	UHWCON |= (1<<UVREGE);

	// Configure PLL (setup 48 MHz USB clock)
	PLLCSR = 0;
	// Set PINDIV because we are using 16 MHz crystal
	PLLCSR |= (1<<PINDIV);
	// Configure 96MHz PLL output (is then divided by 2 to get 48 MHz USB clock)
	PLLFRQ = (1<<PDIV3) | (1<<PDIV1) | (1<<PLLUSB) | (1 << PLLTM0);
	// Enable PLL
	PLLCSR |= (1<<PLLE);
	
	// Wait for PLL lock
	while (!(PLLCSR & (1<<PLOCK)))
		;
			
	// Enable USB
	USBCON |= (1<<USBE)|(1<<OTGPADE);
	// Clear freeze clock bit
	USBCON &= ~(1<<FRZCLK);
	
	// configure USB interface (speed, endpoints, etc.)
	UDCON &= ~(1 << LSM);     // full speed 12 Mbit/s
	
	// disable rest of endpoints
	for (uint8_t i = 1; i <= 6; i++) {
		UENUM = (UENUM & 0xF8) | i;   // select endpoint i
		UECONX &= ~(1 << EPEN);
	}	
}

enum ustate{usDisconnected, usDone};

int main(void)
{
	// Enable interrupts
	USBCON=0;

	sei();
	
	DDRC = 0x80;
	
	USART_Init();

	// Print startup message
	printf_P(PSTR("Reboot!\r\n"));

	// Configure PLL, USB
	setup_usb();

	unsigned int loop_ctr(0);

    ustate us(usDisconnected);

    // Main loop
    while (1) 
    {
			++loop_ctr;

			// Blink the yellow LED on the Leonardo board,
			// so we can tell the main loop is running or not.
			if (loop_ctr&0x80)
				set_bit(PORTC,PORTC7);
			else		
				clear_bit(PORTC,PORTC7);
				
			_delay_ms(5);

			switch (us) {
			case usDisconnected:				
				if ((USBSTA & (1 << VBUS))) {
					printf_P(PSTR("Plugged in!\r\n"));
					// connected
					UDCON &= ~(1 << DETACH);
				
					//end of reset interrupts
					UDIEN |= (1<<EORSTE);//enable the end of reset interrupt
						
					us = usDone;
				}
				break;
					
			case usDone:
				// TODO / BUG: This condition never seems to be met, at least with my Arduino Leonardo board
				if (!((USBSTA & (1 << VBUS)))) {
					// we got disconnected from the pc/laptop
					printf_P(PSTR("Disconnected!\r\n"));
					us = usDisconnected;
				}
				break;				
		}

		// Handle USB control messages
		EP_select(0);
		if ((UEINTX & (1 << RXSTPI)))
			handle_CONTROL();

		// Receive bytes from USB host (laptop/pc)
		handle_incoming_bytes();
		
		// Send bytes to USB host (laptop/pc)
		handle_outgoing_bytes();
    }
}
