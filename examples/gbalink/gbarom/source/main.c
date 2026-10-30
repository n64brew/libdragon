#include <gba.h>
#include <stdio.h>

#define GBA_CHALLENGE 0x64646464
#define GBA_RESPONSE 0x6BA006BA

/* Register REG_JSTAT:
* Bit 1 is SET and immediately RESET (?) when the console sends a Write command over Joybus.
* Bit 3 is SET when the GBA writes to REG_JOYTR (indicating that data is ready).
* Bit 3 is RESET when the console sends a Read command over Joybus.
*/

/*
* Register REG_HS_CTRL:
* Bit 1 is SET when the console sends a Write command over Joybus.
* Bit 2 is SET when the console sends a Read command over Joybus.
* The GBA acknowledges a Write or Read command by writing a 1 to the respective bit.
*/
#define JOY_WRITE_ACK 2
#define JOY_READ_ACK 4

int main(void) {
	irqInit();

	consoleDemoInit();
	iprintf("\x1b[3;2HGBA Link Cable Controller\n");

	// Enable JOY BUS mode
	REG_JOYTR = 0;
	REG_HS_CTRL = JOY_READ_ACK | JOY_WRITE_ACK;
	REG_RCNT = R_JOYBUS;
	
	while (true) {
		// Handle Challenge-Response
		if (REG_JOYRE == GBA_CHALLENGE && REG_HS_CTRL & JOY_WRITE_ACK) {
			// Received challenge, prepare response and wait for read command
			REG_JOYTR = GBA_RESPONSE;
			iprintf("\x1b[5;2H  Waiting for N64...  \n");
			while (!(REG_HS_CTRL & JOY_READ_ACK)) {}
			// Acknowledge write and read commands, and clear data
			REG_HS_CTRL = JOY_READ_ACK | JOY_WRITE_ACK;
			REG_JOYTR = 0;
		}

		// Otherwise, always send held keys
		u16 keys = (REG_KEYINPUT & 0x03ff) ^ 0x03ff;
		REG_JOYTR = (u32) keys;
		iprintf("\x1b[5;2H   0x%04x / 0x%08lx\n", keys, (u32) keys);
		if(REG_HS_CTRL & JOY_READ_ACK) {
			// Acknowledge read command
			REG_HS_CTRL = JOY_READ_ACK;
		}
	}
}
