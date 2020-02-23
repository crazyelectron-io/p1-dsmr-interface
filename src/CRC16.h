#ifndef CRC16_H
#define CRC16_H

#include "Arduino.h"

unsigned int Crc16(unsigned int uCrc, unsigned char *pchBuf, int nLen)
{
	for (int pos = 0; pos < nLen; pos++)
	{
		uCrc ^= (unsigned int)pchBuf[pos];  // XOR byte into least sig. byte of crc

		for (int i = 8; i != 0; i--) {    	// Loop over each bit
			if ((uCrc & 0x0001) != 0) {     // If the LSB is set
				uCrc >>= 1;                 // Shift right and XOR 0xA001
				uCrc ^= 0xA001;
			}
			else                            // Else LSB is not set
				uCrc >>= 1;                 // Just shift right
		}
	}

	return uCrc;
}
#endif
