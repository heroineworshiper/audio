#ifndef ZOOM_H
#define ZOOM_H

// the modes for monitoring & recording

// left I2S2, right from I2S3 with differential inputs
// single monitor volume used
#define MONITOR_2CH_DIFF 0

// left right I2S2, left right I2S3 with single ended inputs
// 2 monitor_volumes used
#define MONITOR_4CH 1

// left right I2S2 with single ended inputs, mono I2S3 with differential inputs
// 2 monitor_volumes used
#define MONITOR_3CH 2

// left I2S2, right I2S3 with averaged single ended inputs
// single monitor volume used
#define MONITOR_2CH_AVG 3

// I2S3 differential inputs.  
// Use MONITOR_2CH_AVG for channel averaging, because the 2 I2S's are out of sync
#define MONITOR_1CH_DIFF 4

// left right I2S3 with single ended inputs
// 2 monitor_volumes used
#define MONITOR_2CH 5

#endif


