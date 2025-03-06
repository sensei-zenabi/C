/*
 * object_recognition.c
 *
 * This module detects and tracks objects in video frames from a 320×240 camera.
 * It updates a background model, detects moving regions, reduces noise, and
 * calculates the center position of detected motion. An optional crosshair is
 * drawn at the calculated position.
 *
 * Implementation details:
 *   - Uses a running average to update the background model.
 *   - Detects motion by comparing the current frame against the background.
 *   - Applies a 3×3 erosion followed by a 3×3 dilation to remove noise.
 *   - Calculates the center of mass of the moving pixels.
 *   - Written in plain C (using -std=c11) with only standard libraries.
 *   - All code is in one file.
 *
 * Optimizations for 320×240 resolution:
 *   1. Resolution-specific constants.
 *   2. Static allocation of buffers to avoid repeated memory allocation.
 *   3. Tuned parameters (e.g., motion threshold, crosshair size) for this resolution.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Camera resolution settings for a 320×240 camera */
#define CAM_WIDTH    320
#define CAM_HEIGHT   240

// For YUYV format, the Y channel is used from paired pixels.
// GRID_WIDTH is half of CAM_WIDTH, GRID_HEIGHT is equal to CAM_HEIGHT.
#define GRID_WIDTH   (CAM_WIDTH / 2)
#define GRID_HEIGHT  (CAM_HEIGHT)
#define GRID_SIZE    (GRID_WIDTH * GRID_HEIGHT)

/* Parameters for motion detection and background update */
#define MOTION_THRESHOLD 1            // Threshold for detecting motion in the Y channel
#define BG_MOTION_DIFF_THRESHOLD 1.0f  // Difference above which the background is updated faster
#define BG_ALPHA_NO_MOTION 0.001f      // Slow update rate for the background when there is little change
#define BG_ALPHA_MOTION 0.010f         // Fast update rate for the background when there is significant change

// Adaptive threshold: adjust the motion threshold based on local brightness.
#define ENABLE_ADAPTIVE_THRESHOLD 1    // Set to 0 to disable adaptive thresholding
#define ADAPTIVE_FACTOR 0.2f           // Fraction used to adjust the threshold based on the background

/* Drawing parameters tuned for 320×240 resolution */
#define CROSSHAIR_SIZE 10              // Size of the crosshair marker
#define MIN_MOVEMENT_PIXELS 50         // Minimum number of motion pixels required

// Define the red color (used to overlay detected motion) in YUYV format.
const unsigned char redY = 76;
const unsigned char redU = 84;
const unsigned char redV = 255;

typedef struct {
    unsigned char Y;
    unsigned char U;
    unsigned char V;
} Color;

/* Color used for drawing the crosshair marker */
Color marker_color = {41, 240, 110};

/* 
 * Static buffers for processing frames.
 * These are allocated once to avoid repeated dynamic memory allocation.
 */
static unsigned char *orig_frame = NULL;      // Copy of the input frame
static float *backgroundY = NULL;             // Background model (Y channel)
static int *motion_mask = NULL;               // Binary mask for detected motion
static int *eroded_mask = NULL;               // Buffer for the erosion step
static int *dilated_mask = NULL;              // Buffer for the dilation step
static int last_center_x = CAM_WIDTH / 2;       // Last known x-coordinate of the motion center
static int last_center_y = CAM_HEIGHT / 2;      // Last known y-coordinate of the motion center

/**********************************************************
 * set_pixel
 *
 * Updates the color of a single pixel at (x, y) in the frame.
 * The frame is in YUYV format where pixels are stored in pairs.
 **********************************************************/
void set_pixel(unsigned char *frame, int frame_width, int frame_height, int x, int y, Color color) {
    if (x < 0 || x >= frame_width || y < 0 || y >= frame_height)
        return;
    int pair_index = x / 2;
    int row_pairs = frame_width / 2;
    int offset = (y * row_pairs + pair_index) * 4;
    frame[offset]     = color.Y;
    frame[offset + 1] = color.U;
    frame[offset + 2] = color.Y;
    frame[offset + 3] = color.V;
}

/**********************************************************
 * draw_crosshair
 *
 * Draws a crosshair at the specified center (center_x, center_y).
 * It draws a horizontal and a vertical line with a fixed size.
 **********************************************************/
void draw_crosshair(unsigned char *frame, int frame_width, int frame_height,
                    int center_x, int center_y, Color color) {
    // Draw horizontal line
    for (int x = center_x - CROSSHAIR_SIZE; x <= center_x + CROSSHAIR_SIZE; x++) {
        set_pixel(frame, frame_width, frame_height, x, center_y, color);
    }
    // Draw vertical line
    for (int y = center_y - CROSSHAIR_SIZE; y <= center_y + CROSSHAIR_SIZE; y++) {
        set_pixel(frame, frame_width, frame_height, center_x, y, color);
    }
}

/**********************************************************
 * process_frame
 *
 * Processes a video frame by performing these steps:
 * 1. On the first call, allocate buffers and initialize the background.
 * 2. For each grid cell:
 *    - Calculate the average brightness (Y channel) from paired pixels.
 *    - Compare it to the background and update the background model.
 *    - Mark the cell as "motion" if the difference exceeds a threshold.
 * 3. Reduce noise using a 3×3 erosion followed by a 3×3 dilation.
 * 4. Calculate the center of mass for the motion pixels.
 * 5. Draw a crosshair at the detected (or last known) center if enough motion is detected.
 *
 * The function assumes the frame is in YUYV format.
 **********************************************************/
void process_frame(unsigned char *frame, size_t frame_size, int frame_width, int frame_height) {
    // Allocate buffers on the first call and initialize the background model.
    if (orig_frame == NULL) {
        orig_frame = malloc(frame_size);
        backgroundY = malloc(GRID_SIZE * sizeof(float));
        motion_mask = malloc(GRID_SIZE * sizeof(int));
        eroded_mask = malloc(GRID_SIZE * sizeof(int));
        dilated_mask = malloc(GRID_SIZE * sizeof(int));
        if (!orig_frame || !backgroundY || !motion_mask || !eroded_mask || !dilated_mask) {
            fprintf(stderr, "Initialization: Out of memory.\n");
            exit(EXIT_FAILURE);
        }
        // Initialize background model from the first frame.
        memcpy(orig_frame, frame, frame_size);
        for (int i = 0; i < GRID_SIZE; i++) {
            int base = i * 4;
            float y1 = (float)frame[base];
            float y2 = (float)frame[base + 2];
            backgroundY[i] = (y1 + y2) / 2.0f;
        }
        return;
    }

    // Keep a copy of the original frame.
    memcpy(orig_frame, frame, frame_size);

    /* Step 1 & 2: Update background model and mark motion.
     * For each grid cell:
     *   - Compute the average brightness from the paired pixels.
     *   - Calculate the absolute difference from the background.
     *   - Choose a fast or slow background update rate based on the difference.
     *   - Optionally adjust the threshold based on the background brightness.
     *   - If the difference exceeds the threshold, mark the cell as motion and overlay red.
     */
    for (int i = 0; i < GRID_SIZE; i++) {
        int base = i * 4;
        float y1 = (float)orig_frame[base];
        float y2 = (float)orig_frame[base + 2];
        float currentY = (y1 + y2) / 2.0f;

        float diff = fabsf(currentY - backgroundY[i]);
        float alpha = BG_ALPHA_NO_MOTION;
        if (diff > BG_MOTION_DIFF_THRESHOLD) {
            alpha = BG_ALPHA_MOTION;
        }
        backgroundY[i] = alpha * backgroundY[i] + (1.0f - alpha) * currentY;

        float dynamic_thresh = MOTION_THRESHOLD;
#if ENABLE_ADAPTIVE_THRESHOLD
        // Adjust threshold based on the background brightness.
        float adaptive_component = ADAPTIVE_FACTOR * (backgroundY[i] + 1.0f);
        if (adaptive_component > dynamic_thresh) {
            dynamic_thresh = adaptive_component;
        }
#endif
        if (diff > dynamic_thresh) {
            // Mark cell as motion and overlay red.
            frame[base]   = redY;
            frame[base+1] = redU;
            frame[base+2] = redY;
            frame[base+3] = redV;
            motion_mask[i] = 1;
        } else {
            // No motion: restore the original pixel.
            frame[base]   = orig_frame[base];
            frame[base+1] = orig_frame[base+1];
            frame[base+2] = orig_frame[base+2];
            frame[base+3] = orig_frame[base+3];
            motion_mask[i] = 0;
        }
    }

    /* Step 3: Noise reduction using a 3×3 erosion followed by dilation.
     * Erosion: A cell is marked as motion only if all its 3×3 neighbors are marked.
     * Dilation: Expand motion regions by marking cells that are adjacent to motion.
     */
    memset(eroded_mask, 0, GRID_SIZE * sizeof(int));
    for (int y = 1; y < GRID_HEIGHT - 1; y++) {
        for (int x = 1; x < GRID_WIDTH - 1; x++) {
            int idx = y * GRID_WIDTH + x;
            int all_one = 1;
            for (int ny = y - 1; ny <= y + 1; ny++) {
                for (int nx = x - 1; nx <= x + 1; nx++) {
                    int nidx = ny * GRID_WIDTH + nx;
                    if (motion_mask[nidx] == 0) {
                        all_one = 0;
                        break;
                    }
                }
                if (!all_one)
                    break;
            }
            eroded_mask[idx] = all_one;
        }
    }
    memset(dilated_mask, 0, GRID_SIZE * sizeof(int));
    for (int y = 1; y < GRID_HEIGHT - 1; y++) {
        for (int x = 1; x < GRID_WIDTH - 1; x++) {
            int idx = y * GRID_WIDTH + x;
            if (eroded_mask[idx] == 1) {
                dilated_mask[idx] = 1;
            } else {
                int found = 0;
                for (int ny = y - 1; ny <= y + 1 && !found; ny++) {
                    for (int nx = x - 1; nx <= x + 1; nx++) {
                        int nidx = ny * GRID_WIDTH + nx;
                        if (eroded_mask[nidx] == 1) {
                            found = 1;
                            break;
                        }
                    }
                }
                dilated_mask[idx] = found;
            }
        }
    }

    /* Step 4: Calculate the center of mass of the motion.
     * Sum the coordinates of all cells marked as motion in the dilated mask.
     * If the number of motion cells is at least MIN_MOVEMENT_PIXELS, update the center.
     * Convert grid coordinates to frame coordinates (x is scaled by 2 and offset).
     */
    long sum_x = 0, sum_y = 0;
    int count = 0;
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++) {
            int idx = y * GRID_WIDTH + x;
            if (dilated_mask[idx] == 1) {
                sum_x += x;
                sum_y += y;
                count++;
            }
        }
    }
    if (count >= MIN_MOVEMENT_PIXELS) {
        int center_x = (int)((sum_x / (float)count) * 2) + 1;
        int center_y = (int)(sum_y / (float)count);
        last_center_x = center_x;
        last_center_y = center_y;
        fprintf(stderr, "Center-of-mass detected at (%d, %d) with %d motion pixels.\n", center_x, center_y, count);
    } else {
        fprintf(stderr, "Insufficient motion detected (only %d pixels).\n", count);
    }

    /* Step 5: Draw a crosshair at the detected or last known center.
     * The crosshair is drawn only when sufficient motion is detected.
     */
    if (count >= MIN_MOVEMENT_PIXELS) {
        draw_crosshair(frame, frame_width, frame_height, last_center_x, last_center_y, marker_color);
    }
}

/*
 * End of object_recognition.c
 *
 * Notes:
 * - Adaptive thresholding adjusts the motion detection threshold using local brightness.
 * - The parameters (MIN_MOVEMENT_PIXELS, CROSSHAIR_SIZE, etc.) are tuned for a 320×240 camera.
 * - The center-of-mass is calculated after noise reduction.
 */
