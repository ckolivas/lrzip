/*
   Copyright (C) 2006-2011 Con Kolivas
   Copyright (C) 2011 Peter Hyman
   Copyright (C) 1998-2003 Andrew Tridgell

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef LIBLRZIP_H
#define LIBLRZIP_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#ifdef _WIN32
# include <stddef.h>
#else
# include <inttypes.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 @brief LRZIP library

 @mainpage lrzip
 @version 1.0
 @date 2011

 @section intro What is LRZIP?
 
 LRZIP is a compression program optimised for large files. The larger the file
 and the more memory you have, the better the compression advantage this will
 provide, especially once the files are larger than 100MB. The advantage can
 be chosen to be either size (much smaller than bzip2) or speed (much faster
 than bzip2).
 * @link Lrzip.h LRZIP API @endlink
 */

/** @file Lrzip.h */

/**
 * @typedef Lrzip
 * @brief The overall struct for managing all operations
 */
typedef struct Lrzip Lrzip;

/**
 * @typedef Lrzip_Log_Level
 * @brief The amount of information to display using logging functions
 * This enum is used when setting or getting the log level of an #Lrzip
 * struct. It determines how much information is shown about the current operation,
 * either in stdout/stderr or using logging callbacks.
 * @see lrzip_log_level_set()
 * @see lrzip_log_level_get()
 */
typedef enum {
	LRZIP_LOG_LEVEL_ERROR = 0, /**< Only display errors */
	LRZIP_LOG_LEVEL_INFO, /**< Display information and errors */
	LRZIP_LOG_LEVEL_PROGRESS, /**< Display progress updates, information, and errors */
	LRZIP_LOG_LEVEL_VERBOSE, /**< Display verbose progress updates, information, and errors */
	LRZIP_LOG_LEVEL_DEBUG /**< Display all possible information */
} Lrzip_Log_Level;

/**
 * @typedef Lrzip_Mode
 * @brief The mode of operation for an #Lrzip struct
 * This enum is used when setting or getting the operation mode of an #Lrzip
 * struct. It determines what will happen when lrzip_run() is called.
 * @see lrzip_mode_set()
 * @see lrzip_mode_get()
 */
typedef enum {
	LRZIP_MODE_NONE = 0, /**< No operation set */
	LRZIP_MODE_INFO, /**< Retrieve info about an archive */
	LRZIP_MODE_TEST, /**< Test an archive's integrity */
	LRZIP_MODE_DECOMPRESS, /**< Decompress an archive */
	LRZIP_MODE_COMPRESS_NONE, /**< RZIP preprocess only */
	LRZIP_MODE_COMPRESS_LZO, /**< Use LZO compression */
	LRZIP_MODE_COMPRESS_ZLIB, /**< Use ZLIB (GZIP) compression */
	LRZIP_MODE_COMPRESS_BZIP2, /**< Use BZIP2 compression */
	LRZIP_MODE_COMPRESS_LZMA, /**< Use LZMA compression */
	LRZIP_MODE_COMPRESS_ZPAQ /**< Use ZPAQ compression */
} Lrzip_Mode;

/**
 * @typedef Lrzip_Flag
 * @brief The extra params for an #Lrzip struct's operations
 * This enum is used when setting or getting the flags of an #Lrzip
 * struct. It determines some of the miscellaneous extra abilities of LRZIP.
 * @see lrzip_flags_set()
 * @see lrzip_flags_get()
 */
typedef enum {
	LRZIP_FLAG_REMOVE_SOURCE = (1 << 0), /**< Remove the input file after the operation completes */
	LRZIP_FLAG_REMOVE_DESTINATION = (1 << 1), /**< Remove matching destination file if it exists */
	LRZIP_FLAG_KEEP_BROKEN = (1 << 2), /**< Do not remove broken files */
	LRZIP_FLAG_VERIFY = (1 << 3), /**< Only verify the archive, do not perform any compression/decompression */
	LRZIP_FLAG_DISABLE_LZO_CHECK = (1 << 4), /**< Disable test to determine if LZO compression will be useful */
	LRZIP_FLAG_UNLIMITED_RAM = (1 << 5), /**< Use unlimited ram window size for compression */
	LRZIP_FLAG_ENCRYPT = (1 << 6) /**< Encrypt archive during compression; @see lrzip_pass_cb_set() */
} Lrzip_Flag;

/**
 * @typedef Lrzip_Info_Cb
 * @brief The callback to call when an operation's progress changes
 * @param data The data param passed in lrzip_info_cb_set()
 * @param pct The overall operation progress as a percent
 * @param chunk_pct The current chunk's operation progress as a percent
 */
typedef void (*Lrzip_Info_Cb)(void *data, int pct, int chunk_pct);
/**
 * @typedef Lrzip_Log_Cb
 * @brief The callback to call when a log message is to be shown
 * @param data The data param passed in lrzip_log_cb_set()
 * @param level The Lrzip_Log_Level of the message
 * @param line The line in LRZIP code where the message originated
 * @param file The file in LRZIP code where the message originated
 * @param func The function in LRZIP code where the message originated
 * @param format The printf-style format of the message
 * @param args The matching va_list for @p format
 */
typedef void (*Lrzip_Log_Cb)(void *data, unsigned int level, unsigned int line, const char *file, const char *func, const char *format, va_list args);
/**
 * @typedef Lrzip_Password_Cb
 * @brief The callback to call for operations requiring a password
 * @param data The data param passed in lrzip_pass_cb_set()
 * @param buffer The pre-allocated buffer to write the password into
 * @param buf_size The size, in bytes, of @p buffer
 */
typedef void (*Lrzip_Password_Cb)(void *data, char *buffer, size_t buf_size);

/**
 * @brief Initialize liblrzip
 * This function must be called prior to running any other liblrzip
 * functions to initialize compression algorithms. It does not allocate.
 * @return true on success, false on failure
 */
bool lrzip_init(void);

/**
 * @brief Create a new #Lrzip struct
 * Use this function to allocate a new struct for immediate or later use,
 * optionally setting flags and changing modes at a later time.
 * @param mode The optional Lrzip_Mode to set, or LRZIP_MODE_NONE to allow
 * setting a mode later.
 * @return The new #Lrzip struct, or NULL on failure
 * @see lrzip_mode_set()
 */
Lrzip *lrzip_new(Lrzip_Mode mode);

/**
 * @brief Free an #Lrzip struct
 * Use this function to free all memory associated with an existing struct.
 * @param lr The struct to free
 */
void lrzip_free(Lrzip *lr);

/**
 * @brief Set up an #Lrzip struct using environment settings
 * Use this function to acquire and utilize settings already existing in
 * either environment variables or configuration files for LRZIP. For more detailed
 * information, see the LRZIP manual.
 * @param lr The struct to configure
 * @note This function cannot fail.
 */
void lrzip_config_env(Lrzip *lr);

/**
 * @brief Retrieve the operation mode of an #Lrzip struct
 * @param lr The struct to query
 * @return The Lrzip_Mode of @p lr, or LRZIP_MODE_NONE on failure
 */
Lrzip_Mode lrzip_mode_get(Lrzip *lr);

/**
 * @brief Set the operation mode of an #Lrzip struct
 * @param lr The struct to change the mode for
 * @param mode The Lrzip_Mode to set for @p lr
 * @return true on success, false on failure
 */
bool lrzip_mode_set(Lrzip *lr, Lrzip_Mode mode);

/**
 * @brief Set the compression level of an #Lrzip struct
 * @param lr The struct to change the compression level for
 * @param level The value, 1-9, to use as the compression level for operations with @p lr
 * @return true on success, false on failure
 * @note This function is only valid for compression operations
 */
bool lrzip_compression_level_set(Lrzip *lr, unsigned int level);

/**
 * @brief Get the compression level of an #Lrzip struct
 * @param lr The struct to get the compression level of
 * @return The value, 1-9, used as the compression level for operations with @p lr,
 * or 0 on failure
 * @note This function is only valid for compression operations
 */
unsigned int lrzip_compression_level_get(Lrzip *lr);

/**
 * @brief Set the operation specific parameters
 * @param lr The struct to set parameters for
 * @param flags A bitwise ORed set of Lrzip_Flags
 * @note This function does not perform any error checking. Any errors in flags
 * will be determined when lrzip_run() is called.
 */
void lrzip_flags_set(Lrzip *lr, unsigned int flags);

/**
 * @brief Get the operation specific parameters
 * @param lr The struct to get parameters of
 * @return A bitwise ORed set of Lrzip_Flags
 */
unsigned int lrzip_flags_get(Lrzip *lr);

/**
 * @brief Set the nice level for operations in a struct
 * @param lr The struct to set the nice level for
 * @param nice The value to use when nicing during operations
 */
void lrzip_nice_set(Lrzip *lr, int nice);

/**
 * @brief Get the nice level for operations in a struct
 * @param lr The struct to get the nice level of
 * @return The value to use when nicing during operations
 */
int lrzip_nice_get(Lrzip *lr);

/**
 * @brief Explicitly set the number of threads to use during operations
 * @param lr The struct to set the threads for
 * @param threads The number of threads to use for operations
 * @note LRZIP will automatically determine the optimal number of threads to use,
 * so this function should only be used to specify FEWER than optimal threads.
 */
void lrzip_threads_set(Lrzip *lr, unsigned int threads);

/**
 * @brief Get the number of threads used during operations
 * @param lr The struct to query
 * @return The number of threads to use for operations
 */
unsigned int lrzip_threads_get(Lrzip *lr);

/**
 * @brief Set the maximum compression window for operations
 * @param lr The struct to set the maximum compression window for
 * @param size The size (in hundreds of MB) to use for the maximum size of compression
 * chunks.
 * @note LRZIP will automatically determine the optimal maximum compression window to use,
 * so this function should only be used to specify a LOWER value.
 */
void lrzip_compression_window_max_set(Lrzip *lr, int64_t size);

/**
 * @brief Get the maximum compression window for operations
 * @param lr The struct to query
 * @return The size (in hundreds of MB) to use for the maximum size of compression
 * chunks.
 */
int64_t lrzip_compression_window_max_get(Lrzip *lr);

/**
 * @brief Return the size of the stream queue in a struct
 * This function returns the current count of streams added for processing
 * using lrzip_file_add. It always returns instantly.
 * @param lr The struct to query
 * @return The current number of streams in the queue
 */
unsigned int lrzip_files_count(Lrzip *lr);

/**
 * @brief Return the size of the file queue in a struct
 * This function returns the current count of files added for processing
 * using lrzip_filename_add. It always returns instantly.
 * @param lr The struct to query
 * @return The current number of files in the queue
 */
unsigned int lrzip_filenames_count(Lrzip *lr);

/**
 * @brief Return the array of the stream queue in a struct
 * This function returns the current queue of streams added for processing
 * using lrzip_file_add. It always returns instantly.
 * @param lr The struct to query
 * @return The current stream queue
 */
FILE **lrzip_files_get(Lrzip *lr);

/**
 * @brief Return the array of the filename queue in a struct
 * This function returns the current queue of files added for processing
 * using lrzip_filename_add. It always returns instantly.
 * @param lr The struct to query
 * @return The current filename queue
 */
char **lrzip_filenames_get(Lrzip *lr);

/**
 * @brief Add a stream (FILE) to the operation queue
 * This function adds a stream to the input queue. Each time lrzip_run()
 * is called, it will run the current operation (specified by the Lrzip_Mode)
 * on either a stream or file in the queue.
 * @param lr The struct
 * @param file The stream descriptor to queue
 * @return true on success, false on failure
 * @note The file queue will be fully processed prior to beginning processing
 * the stream queue.
 * @warning Any streams added to this queue MUST NOT be closed until they have
 * either been processed or removed from the queue!
 */
bool lrzip_file_add(Lrzip *lr, FILE *file);

/**
 * @brief Remove a stream from the operation queue
 * This function removes a previously added stream from the operation queue by
 * iterating through the queue and removing the stream if found.
 * @param lr The struct
 * @param file The stream to remove
 * @return true only on successful removal, else false
 */
bool lrzip_file_del(Lrzip *lr, FILE *file);

/**
 * @brief Pop the current head of the stream queue
 * This function is used to remove the current head of the stream queue. It can be called
 * immediately following any lrzip_run() stream operation to remove the just-processed stream. This
 * function modifies the stream queue array, reordering and updating the index count.
 * @param lr The struct to pop the stream queue of
 * @return The stream removed from the queue, or NULL on failure
 */
FILE *lrzip_file_pop(Lrzip *lr);

/**
 * @brief Clear the stream queue
 * This function is used to free and reset the stream queue. The streams
 * themselves are untouched.
 * @param lr The struct
 */
void lrzip_files_clear(Lrzip *lr);

/**
 * @brief Add a file to the operation queue
 * This function adds a file to the input queue. Each time lrzip_run()
 * is called, it will run the current operation (specified by the Lrzip_Mode)
 * on either a stream or file in the queue.
 * @param lr The struct
 * @param file The file (by absolute path) to queue
 * @return true on success, false on failure
 * @note The file queue will be fully processed prior to beginning processing
 * the stream queue.
 */
bool lrzip_filename_add(Lrzip *lr, const char *file);

/**
 * @brief Remove a filename from the operation queue
 * This function removes a previously added filename from the operation queue by
 * iterating through the queue and removing the filename if found.
 * @param lr The struct
 * @param file The file to remove
 * @return true only on successful removal, else false
 */
bool lrzip_filename_del(Lrzip *lr, const char *file);

/**
 * @brief Pop the current head of the file queue
 * This function is used to remove the current head of the file queue. It can be called
 * immediately following any lrzip_run() file operation to remove the just-processed file. This
 * function modifies the file queue array, reordering and updating the index count.
 * @param lr The struct to pop the filename queue of
 * @return The filename removed from the queue, or NULL on failure
 */
const char *lrzip_filename_pop(Lrzip *lr);

/**
 * @brief Clear the file queue
 * This function is used to free and reset the file queue.
 * @param lr The struct
 */
void lrzip_filenames_clear(Lrzip *lr);

/**
 * @brief Set the default suffix for LRZIP compression operations
 * This function is used to change the default ".lrz" suffix for operations
 * to @p suffix.
 * @param lr The struct
 * @param suffix The suffix to use for compression operations
 */
void lrzip_suffix_set(Lrzip *lr, const char *suffix);

/**
 * @brief Get the default suffix for LRZIP compression operations
 * @param lr The struct
 * @return The suffix to use for compression operations, or NULL on failure
 */
const char *lrzip_suffix_get(Lrzip *lr);

/**
 * @brief Set the output directory for operations
 * This function can be used to set the output directory for operations.
 * Files will be stored according to their basename and lrzip suffix where
 * applicable.
 * @param lr The struct
 * @param dir The absolute path of the output directory
 */
void lrzip_outdir_set(Lrzip *lr, const char *dir);

/**
 * @brief Get the output directory for operations
 * @param lr The struct
 * @return The previously set output directory
 */
const char *lrzip_outdir_get(Lrzip *lr);

/**
 * @brief Set the output stream for operations
 * This function can be used to set the output stream for operations.
 * Raw data will be written to this stream for the duration of lrzip_run().
 * @param lr The struct
 * @param file The stream to write to
 * @warning @p file is NOT created by this library and must be opened by the user!
 */
void lrzip_outfile_set(Lrzip *lr, FILE *file);

/**
 * @brief Get the output stream for operations
 * @param lr The struct
 * @return The previously set output stream
 */
FILE *lrzip_outfile_get(Lrzip *lr);

/**
 * @brief Set the output file for operations
 * This function can be used to set the output file for operations.
 * Raw data will be written to the file with this name for the duration of lrzip_run().
 * @param lr The struct
 * @param file The name of the file to write to
 */
void lrzip_outfilename_set(Lrzip *lr, const char *file);

/**
 * @brief Get the output filename for operations
 * @param lr The struct
 * @return The previously set output filename
 */
const char *lrzip_outfilename_get(Lrzip *lr);

/**
 * @brief Retrieve the MD5 digest of an LRZIP file
 * Use this function after calling lrzip_run() to retrieve the digest of
 * the processed archive.
 * @param lr The struct having run an operation
 * @return The MD5 digest of the operation's associated archive
 * @note The return value of this function will change after each operation
 */
const unsigned char *lrzip_md5digest_get(Lrzip *lr);

/**
 * @brief Run the current operation
 * This function is called when all necessary parameters have been set for an operation.
 * The calling thread will then block until the operation has fully completed, writing
 * output using logging and progress callbacks and calling password callbacks as required.
 * @param lr The struct to run an operation with
 * @return true if the operation successfully completed, else false
 */
bool lrzip_run(Lrzip *lr);

/**
 * @brief Set the logging level
 * @param lr The struct
 * @param level The #Lrzip_Log_Level to use
 */
void lrzip_log_level_set(Lrzip *lr, int level);

/**
 * @brief Get the logging level
 * @param lr The struct to query
 * @return The #Lrzip_Log_Level of @p lr
 */
int lrzip_log_level_get(Lrzip *lr);

/**
 * @brief Set a logging callback for use with all operations
 * This function sets an Lrzip_Log_Cb which will be called any time logging
 * output is to be displayed. The callback will be called as many times as the #Lrzip_Log_Level
 * requires.
 * @param lr The struct
 * @param cb The callback
 * @param log_data The data param to use in the logging callback
 */
void lrzip_log_cb_set(Lrzip *lr, Lrzip_Log_Cb cb, void *log_data);

/**
 * @brief Redirect stdout log messages to another stream
 * This function sends any logging messages which would normally go to stdout into another stream.
 * Useful for when stdout is the target set by lrzip_outfile_set().
 * @param lr The struct
 * @param out The stream to use instead of stdout
 */
void lrzip_log_stdout_set(Lrzip *lr, FILE *out);

/**
 * @brief Return the stream currently used as stdout
 * @param lr The struct to query
 * @return A stream where stdout messages will be sent, NULL on failure
 */
FILE *lrzip_log_stdout_get(Lrzip *lr);

/**
 * @brief Redirect stderr log messages to another stream
 * This function sends any logging messages which would normally go to stderr into another stream.
 * @param lr The struct
 * @param err The stream to use instead of stderr
 */
void lrzip_log_stderr_set(Lrzip *lr, FILE *err);

/**
 * @brief Return the stream currently used as stderr
 * @param lr The struct to query
 * @return A stream where stderr messages will be sent, NULL on failure
 */
FILE *lrzip_log_stderr_get(Lrzip *lr);

/**
 * @brief Set a password callback for use with all operations
 * This function sets an Lrzip_Password_Cb which will be used when working with encrypted
 * LRZIP archives. It will be called both when compressing and decompressing archives.
 * @param lr The struct
 * @param cb The callback to set
 * @param data The data param to use in the password callback
 */
void lrzip_pass_cb_set(Lrzip *lr, Lrzip_Password_Cb cb, void *data);

/**
 * @brief Set an info callback for use with all operations
 * This function sets an Lrzip_Info_Cb which will be called any time there is a
 * progress update in an operation.
 * @param lr The struct
 * @param cb The callback to set
 * @param data The data param to use in the info callback
 */
void lrzip_info_cb_set(Lrzip *lr, Lrzip_Info_Cb cb, void *data);

/**
 * @brief Quick setup for performing a decompression
 * This function performs all the required allocations and sets necessary parameters
 * to decompress @p source to @p dest. No extra functions are necessary to call, and
 * this function will block until it completes.
 * @param dest A pointer to the LRZIP-allocated destination buffer
 * @param dest_len A pointer to the length of @p dest
 * @param source The allocated source buffer to read from
 * @param source_len The length of @p source
 * @return true on success, else false
 */
bool lrzip_decompress(void *dest, unsigned long *dest_len, const void *source, unsigned long source_len);

/**
 * @brief Quick setup for performing a compression
 * This function performs all the required allocations and sets necessary parameters
 * to compress @p source to @p dest. No extra functions are necessary to call, and
 * this function will block until it completes.
 * @param dest A pointer to the LRZIP-allocated destination buffer
 * @param dest_len A pointer to the length of @p dest
 * @param source The allocated source buffer to read from
 * @param source_len The length of @p source
 * @param mode The compression mode to use
 * @param compress_level The value, 1-9, to use as a compression level
 * @return true on success, else false
 */
bool lrzip_compress_full(void *dest, unsigned long *dest_len, const void *source, unsigned long source_len, Lrzip_Mode mode, int compress_level);

/**
 * @brief Quick setup for performing a compression using LZMA
 * This function performs all the required allocations and sets necessary parameters
 * to compress @p source to @p dest. No extra functions are necessary to call, and
 * this function will block until it completes.
 * @param dest A pointer to the LRZIP-allocated destination buffer
 * @param dest_len A pointer to the length of @p dest
 * @param source The allocated source buffer to read from
 * @param source_len The length of @p source
 * @return true on success, else false
 */
static inline bool lrzip_compress(void *dest, unsigned long *dest_len, const void *source, unsigned long source_len)
{ return lrzip_compress_full(dest, dest_len, source, source_len, LRZIP_MODE_COMPRESS_LZMA, 7); }

/**
 * @brief Quick setup for performing a compression using LZO
 * This function performs all the required allocations and sets necessary parameters
 * to compress @p source to @p dest. No extra functions are necessary to call, and
 * this function will block until it completes.
 * @param dest A pointer to the LRZIP-allocated destination buffer
 * @param dest_len A pointer to the length of @p dest
 * @param source The allocated source buffer to read from
 * @param source_len The length of @p source
 * @return true on success, else false
 */
static inline bool lrzip_lcompress(void *dest, unsigned long *dest_len, const void *source, unsigned long source_len)
{ return lrzip_compress_full(dest, dest_len, source, source_len, LRZIP_MODE_COMPRESS_LZO, 7); }

/**
 * @brief Quick setup for performing a compression using ZLIB (GZIP)
 * This function performs all the required allocations and sets necessary parameters
 * to compress @p source to @p dest. No extra functions are necessary to call, and
 * this function will block until it completes.
 * @param dest A pointer to the LRZIP-allocated destination buffer
 * @param dest_len A pointer to the length of @p dest
 * @param source The allocated source buffer to read from
 * @param source_len The length of @p source
 * @return true on success, else false
 */
static inline bool lrzip_gcompress(void *dest, unsigned long *dest_len, const void *source, unsigned long source_len)
{ return lrzip_compress_full(dest, dest_len, source, source_len, LRZIP_MODE_COMPRESS_ZLIB, 7); }

/**
 * @brief Quick setup for performing a compression using ZPAQ
 * This function performs all the required allocations and sets necessary parameters
 * to compress @p source to @p dest. No extra functions are necessary to call, and
 * this function will block until it completes.
 * @param dest A pointer to the LRZIP-allocated destination buffer
 * @param dest_len A pointer to the length of @p dest
 * @param source The allocated source buffer to read from
 * @param source_len The length of @p source
 * @return true on success, else false
 */
static inline bool lrzip_zcompress(void *dest, unsigned long *dest_len, const void *source, unsigned long source_len)
{ return lrzip_compress_full(dest, dest_len, source, source_len, LRZIP_MODE_COMPRESS_ZPAQ, 7); }

/**
 * @brief Quick setup for performing a compression using BZIP
 * This function performs all the required allocations and sets necessary parameters
 * to compress @p source to @p dest. No extra functions are necessary to call, and
 * this function will block until it completes.
 * @param dest A pointer to the LRZIP-allocated destination buffer
 * @param dest_len A pointer to the length of @p dest
 * @param source The allocated source buffer to read from
 * @param source_len The length of @p source
 * @return true on success, else false
 */
static inline bool lrzip_bcompress(void *dest, unsigned long *dest_len, const void *source, unsigned long source_len)
{ return lrzip_compress_full(dest, dest_len, source, source_len, LRZIP_MODE_COMPRESS_BZIP2, 7); }

/**
 * @brief Quick setup for performing RZIP preprocessing
 * This function performs all the required allocations and sets necessary parameters
 * to preprocess @p source to @p dest. No extra functions are necessary to call, and
 * this function will block until it completes.
 * @param dest A pointer to the LRZIP-allocated destination buffer
 * @param dest_len A pointer to the length of @p dest
 * @param source The allocated source buffer to read from
 * @param source_len The length of @p source
 * @return true on success, else false
 */
static inline bool lrzip_rcompress(void *dest, unsigned long *dest_len, const void *source, unsigned long source_len)
{ return lrzip_compress_full(dest, dest_len, source, source_len, LRZIP_MODE_COMPRESS_NONE, 7); }

/**
 * @brief Quick setup for performing a compression using LZMA and a user-defined compression level
 * This function performs all the required allocations and sets necessary parameters
 * to compress @p source to @p dest. No extra functions are necessary to call, and
 * this function will block until it completes.
 * @param dest A pointer to the LRZIP-allocated destination buffer
 * @param dest_len A pointer to the length of @p dest
 * @param source The allocated source buffer to read from
 * @param source_len The length of @p source
 * @param compress_level The value, 1-9, to use as a compression level
 * @return true on success, else false
 */
static inline bool lrzip_compress2(void *dest, unsigned long *dest_len, const void *source, unsigned long source_len, int compress_level)
{ return lrzip_compress_full(dest, dest_len, source, source_len, LRZIP_MODE_COMPRESS_LZMA, compress_level); }

/**
 * @brief Quick setup for performing a compression using LZO and a user-defined compression level
 * This function performs all the required allocations and sets necessary parameters
 * to compress @p source to @p dest. No extra functions are necessary to call, and
 * this function will block until it completes.
 * @param dest A pointer to the LRZIP-allocated destination buffer
 * @param dest_len A pointer to the length of @p dest
 * @param source The allocated source buffer to read from
 * @param source_len The length of @p source
 * @param compress_level The value, 1-9, to use as a compression level
 * @return true on success, else false
 */
static inline bool lrzip_lcompress2(void *dest, unsigned long *dest_len, const void *source, unsigned long source_len, int compress_level)
{ return lrzip_compress_full(dest, dest_len, source, source_len, LRZIP_MODE_COMPRESS_LZO, compress_level); }

/**
 * @brief Quick setup for performing a compression using ZLIB (GZIP) and a user-defined compression level
 * This function performs all the required allocations and sets necessary parameters
 * to compress @p source to @p dest. No extra functions are necessary to call, and
 * this function will block until it completes.
 * @param dest A pointer to the LRZIP-allocated destination buffer
 * @param dest_len A pointer to the length of @p dest
 * @param source The allocated source buffer to read from
 * @param source_len The length of @p source
 * @param compress_level The value, 1-9, to use as a compression level
 * @return true on success, else false
 */
static inline bool lrzip_gcompress2(void *dest, unsigned long *dest_len, const void *source, unsigned long source_len, int compress_level)
{ return lrzip_compress_full(dest, dest_len, source, source_len, LRZIP_MODE_COMPRESS_ZLIB, compress_level); }

/**
 * @brief Quick setup for performing a compression using ZPAQ and a user-defined compression level
 * This function performs all the required allocations and sets necessary parameters
 * to compress @p source to @p dest. No extra functions are necessary to call, and
 * this function will block until it completes.
 * @param dest A pointer to the LRZIP-allocated destination buffer
 * @param dest_len A pointer to the length of @p dest
 * @param source The allocated source buffer to read from
 * @param source_len The length of @p source
 * @param compress_level The value, 1-9, to use as a compression level
 * @return true on success, else false
 */
static inline bool lrzip_zcompress2(void *dest, unsigned long *dest_len, const void *source, unsigned long source_len, int compress_level)
{ return lrzip_compress_full(dest, dest_len, source, source_len, LRZIP_MODE_COMPRESS_ZPAQ, compress_level); }

/**
 * @brief Quick setup for performing a compression using BZIP and a user-defined compression level
 * This function performs all the required allocations and sets necessary parameters
 * to compress @p source to @p dest. No extra functions are necessary to call, and
 * this function will block until it completes.
 * @param dest A pointer to the LRZIP-allocated destination buffer
 * @param dest_len A pointer to the length of @p dest
 * @param source The allocated source buffer to read from
 * @param source_len The length of @p source
 * @param compress_level The value, 1-9, to use as a compression level
 * @return true on success, else false
 */
static inline bool lrzip_bcompress2(void *dest, unsigned long *dest_len, const void *source, unsigned long source_len, int compress_level)
{ return lrzip_compress_full(dest, dest_len, source, source_len, LRZIP_MODE_COMPRESS_BZIP2, compress_level); }

/**
 * @brief Quick setup for performing RZIP preprocessing and a user-defined compression level
 * This function performs all the required allocations and sets necessary parameters
 * to preprocess @p source to @p dest. No extra functions are necessary to call, and
 * this function will block until it completes.
 * @param dest A pointer to the LRZIP-allocated destination buffer
 * @param dest_len A pointer to the length of @p dest
 * @param source The allocated source buffer to read from
 * @param source_len The length of @p source
 * @param compress_level The value, 1-9, to use as a compression level
 * @return true on success, else false
 */
static inline bool lrzip_rcompress2(void *dest, unsigned long *dest_len, const void *source, unsigned long source_len, int compress_level)
{ return lrzip_compress_full(dest, dest_len, source, source_len, LRZIP_MODE_COMPRESS_NONE, compress_level); }

#ifdef __cplusplus
}
#endif

#endif

