#ifndef HEADERDOWNLINK_H_RTIHU
#define HEADERDOWNLINK_H_RTIHU
//Created by GenFlightStruct.py from file DownlinkSpecGolf-T.xlsx
// Do not edit this file.
#ifndef RTIHU
#error Wrong Archtecture
#endif
typedef struct  __attribute__((__packed__)) _header_t {
    uint16_t resetCnt;       //Offset=0
    uint16_t uptime[2];       //Offset=16
    uint8_t protocolVersion;       //Offset=48
    uint8_t versionMajor;       //Offset=56
    uint8_t versionMinor;       //Offset=64
    unsigned int pad17:5;       //Offset=72
    unsigned int inScienceMode:1;       //Offset=77
    unsigned int inHealthMode:1;       //Offset=78
    unsigned int inSafeMode:1;       //Offset=79
} header_t; // Total Size=80 bits or 10 bytes with 0 left over
#endif
