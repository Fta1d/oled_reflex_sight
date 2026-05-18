#ifndef FRAME_H
#define FRAME_H

#include <stdint.h>

// Wire format (9 bytes):
// [0x55][0xAA][type][bbox_x][bbox_y][bbox_w][bbox_h][pred_x][pred_y]
//
// Coordinates are pre-scaled to OLED space (x: 0-127, y: 0-63) by the sender.

#define FRAME_SYNC0         0x55
#define FRAME_SYNC1         0xAA
#define FRAME_SIZE          9

#define FRAME_TYPE_TARGET     0x01  // drone visible — render bbox + predicted point
#define FRAME_TYPE_LOST       0x02  // target lost  — hide overlays
#define FRAME_TYPE_CROSSHAIR  0x03  // reposition crosshair; bbox_x=cx, bbox_y=cy (other fields ignored)
#define FRAME_TYPE_ARROW      0x04  // guidance arrow: bbox_x=tip_x, bbox_y=tip_y; base drawn from crosshair center
#define FRAME_TYPE_HOLD       0x05  // aim is on predicted target — hold position
#define FRAME_TYPE_HOLD_ARROW   0x06  // hold border + guidance circle; bbox_x=tip_x, bbox_y=tip_y
#define FRAME_TYPE_SET_HOLD     0x07  // resize hold frame: bbox_x=inset_x, bbox_y=inset_y (others ignored)
#define FRAME_TYPE_SET_CIRCLE   0x08  // resize arrow-tip circle: bbox_x=diameter (others ignored)

typedef struct __attribute__((packed)) {
    uint8_t sync[2];    // 0x55, 0xAA
    uint8_t type;       // FRAME_TYPE_*
    uint8_t bbox_x;     // bounding box top-left x  (OLED coords 0-127)
    uint8_t bbox_y;     // bounding box top-left y  (OLED coords 0-63)
    uint8_t bbox_w;     // bounding box width
    uint8_t bbox_h;     // bounding box height
    uint8_t pred_x;     // predicted lead point x   (OLED coords 0-127)
    uint8_t pred_y;     // predicted lead point y   (OLED coords 0-63)
} frame_t;

#endif // FRAME_H
