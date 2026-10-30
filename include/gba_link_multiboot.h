/**
 * @file gba_link_multiboot.h
 * @author tfmoe__ <https://github.com/marian-m12l>
 * @brief GBA Link Cable support for Multi Boot
 * @ingroup gba_link
 */

#ifndef LIBDRAGON_GBA_LINK_MULTIBOOT_H
#define LIBDRAGON_GBA_LINK_MULTIBOOT_H

#include <stdbool.h>

#include "preview.h"

/**
 * @addtogroup gba_link
 * @{
 */

#ifdef __cplusplus
extern "C" {
#endif


/** @brief GBA Multi Boot status values */
typedef enum
{
    /** @brief GBA Multi Boot was successful. */
    GBA_LINK_MULTIBOOT_OK        = 0,
    /** @brief GBA Multi Boot failed because of an issue in I/O operations. */
    GBA_LINK_MULTIBOOT_BAD_IO,
    /** @brief GBA Multi Boot failed because the GBA reported a status inconsistent with I/O operations. */
    GBA_LINK_MULTIBOOT_BAD_STATUS,
    /** @brief GBA Multi Boot failed because the GBA reported a dummy byte inconsistent with uploaded data. */
    GBA_LINK_MULTIBOOT_BAD_DUMMY_BYTE,
    /** @brief GBA Multi Boot failed because of an issue while reading file. */
    GBA_LINK_MULTIBOOT_BAD_FILE
} gba_link_multiboot_status_t;

/** @brief GBA Multi Boot logo base color */
typedef enum {
    /** @brief Pink base color.
	* @note Transitions to blue when combined with a positive speed.
	* @note Transitions from blue when combined with a negative speed.
	*/
	GBA_LINK_MULTIBOOT_LOGO_COLOR_0_PINK = 0,
    /** @brief Blue base color.
	* @note Transitions to cyan when combined with a positive speed.
	* @note Transitions from cyan when combined with a negative speed.
	*/
	GBA_LINK_MULTIBOOT_LOGO_COLOR_1_BLUE,
    /** @brief Cyan base color.
	* @note Transitions to green when combined with a positive speed.
	* @note Transitions from green when combined with a negative speed.
	*/
	GBA_LINK_MULTIBOOT_LOGO_COLOR_2_CYAN,
    /** @brief Green base color.
	* @note Transitions to yellow when combined with a positive speed.
	* @note Transitions from yellow when combined with a negative speed.
	*/
	GBA_LINK_MULTIBOOT_LOGO_COLOR_3_GREEN,
    /** @brief Yellow base color.
	* @note Transitions to red when combined with a positive speed.
	* @note Transitions from red when combined with a negative speed.
	*/
	GBA_LINK_MULTIBOOT_LOGO_COLOR_4_YELLOW,
    /** @brief Red base color.
	* @note Transitions to pink when combined with a positive speed.
	* @note Transitions from pink when combined with a negative speed.
	*/
	GBA_LINK_MULTIBOOT_LOGO_COLOR_5_RED,
    /** @brief Pink base color.
	* @note Transitions to white when combined with a positive speed.
	* @note Transitions from white when combined with a negative speed.
	*/
	GBA_LINK_MULTIBOOT_LOGO_COLOR_6_PINK,
} gba_link_multiboot_logo_color_t;

/** @brief GBA Multi Boot logo speed
* 
* Null speed show base color only.
* Positive speeds animate the logo, transition from the base color.
* Negative speeds animate the logo, transition to the base color.
*/
typedef enum {
	GBA_LINK_MULTIBOOT_LOGO_SPEED_MINUS_4 = -4,
	GBA_LINK_MULTIBOOT_LOGO_SPEED_MINUS_3,
	GBA_LINK_MULTIBOOT_LOGO_SPEED_MINUS_2,
	GBA_LINK_MULTIBOOT_LOGO_SPEED_MINUS_1,
	GBA_LINK_MULTIBOOT_LOGO_SPEED_0,
	GBA_LINK_MULTIBOOT_LOGO_SPEED_PLUS_1,
	GBA_LINK_MULTIBOOT_LOGO_SPEED_PLUS_2,
	GBA_LINK_MULTIBOOT_LOGO_SPEED_PLUS_3,
	GBA_LINK_MULTIBOOT_LOGO_SPEED_PLUS_4,
} gba_link_multiboot_logo_speed_t;

/**
 * @brief GBA Multi Boot callback function type.
 * 
 * This function is called upon completion of a Multi Boot rom upload operation
 * initiated by gba_link_multiboot_start().
 * 
 * @param port              The Joybus port the GBA is connected to.
 * @param status			Multiboot execution status
 * @param ctx               User-defined context pointer.
 */
typedef void (*gba_link_multiboot_callback_t)(int port, gba_link_multiboot_status_t status, void *ctx);

/**
 * @brief Starts the process of asynchronously uploading a rom via GBA Link Cable.
 * The GBA on the given port must be ready for Multi Boot.
 * 
 * @param port 				The Joybus port the GBA is connected to.
 * @param filename 			Path to the GBA rom file to upload.
 * @param logo_color		Color setting for the GBA logo displayed during upload.
 * @param logo_speed		Speed setting for the GBA logo displayed during upload.
 * @param callback 			A function pointer to call when the operation completes.
 * @param ctx 				A user data pointer to pass into the callback function.
 */
LIBDRAGON_PREVIEW_API
void gba_link_multiboot_start(int port, const char* filename, gba_link_multiboot_logo_color_t logo_color, gba_link_multiboot_logo_speed_t logo_speed, gba_link_multiboot_callback_t callback, void *ctx);


/**
 * @brief GBA Multi Boot detection callback function type.
 * 
 * This function is called upon completion of an async detection operation
 * initiated by gba_link_multiboot_detect().
 * 
 * @param port              The Joybus port on which GBA detection happened.
 * @param multiboot_ready   Wheteher a GBA Link Cable is connected to the given port and ready for Multi Boot.
 * @param ctx               User-defined context pointer.
 */
typedef void (*gba_link_multiboot_detect_callback_t)(int port, bool multiboot_ready, void *ctx);

/**
 * @brief Asynchronously detects a GBA Link Cable ready for Multi Boot on the given port.
 * 
 * Repeatedly emits reset and identify commands for a given time, checking the GBA status
 * for Multi Boot readiness.
 * 
 * @param port 				The Joybus port on which to detect GBA Link Cable.
 * @param timeout 			Time (in ms) after which detection is aborted if no GBA Link Cable was identified.
 * @param callback 			A function pointer to call when the operation completes.
 * @param ctx 				A user data pointer to pass into the callback function.
 */
LIBDRAGON_PREVIEW_API
void gba_link_multiboot_detect(int port, int timeout, gba_link_multiboot_detect_callback_t callback, void *ctx);

/**
 * @brief Get the progress of a Multi Boot upload operation on the given port.
 * 
 * @param port 				The Joybus port the GBA is connected to.
 * 
 * @return The progess (in percent), or 0 if there is no Multi Boot operation happening.
 */
LIBDRAGON_PREVIEW_API
uint8_t gba_link_multiboot_progress(int port);


#ifdef __cplusplus
}
#endif

/** @} */ /* gba_link */

#endif /* LIBDRAGON_GBA_LINK_MULTIBOOT_H */
