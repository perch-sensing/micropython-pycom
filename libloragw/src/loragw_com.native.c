/*
/ _____)             _              | |
( (____  _____ ____ _| |_ _____  ____| |__
\____ \| ___ |    (_   _) ___ |/ ___)  _ \
_____) ) ____| | | || |_| ____( (___| | | |
(______/|_____)_|_|_| \__)_____)\____)_| |_|
(C)2013 Semtech-Cycleo

Description:
Host specific functions to address the LoRa concentrator registers through
a SPI interface.
Single-byte read/write and burst read/write.
Does not handle pagination.
Could be used with multiple SPI ports in parallel (explicit file descriptor)

License: Revised BSD License, see LICENSE.TXT file include in the project
Maintainer: Sylvain Miermont


Note : for picogateway this file contains the USB cmd and an encapsulation of the spi functions to provide an usb bridge.
Usb CDC drivers is require to establish the connection with the picogateway.


*/


/* -------------------------------------------------------------------------- */
/* --- DEPENDANCIES --------------------------------------------------------- */

#include <stdint.h>        /* C99 types */
#include <stdio.h>        /* printf fprintf */
#include <stdlib.h>        /* malloc free */
#include <unistd.h>        /* lseek, close */
#include <fcntl.h>        /* open */
#include <string.h>        /* memset */
#include <errno.h>   /* Error number definitions */
#include <termios.h> /* POSIX terminal control definitions */
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <pthread.h>
#include "loragw_com.h"
#include "loragw_hal.h"
#include "loragw_aux.h"
#include "loragw_reg.h"
#include <stdio.h>   /* Standard input/output definitions */
#include <string.h>  /* String function definitions */
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>  /* UNIX standard function definitions */
#include <fcntl.h>   /* File control definitions */
#include <errno.h>   /* Error number definitions */
#include <termios.h> /* POSIX terminal control definitions */
#include <time.h>
#include "loragw_com_linux.h"
#include <sys/select.h>
/* -------------------------------------------------------------------------- */
/* --- PRIVATE MACROS ------------------------------------------------------- */

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#if DEBUG_SPI == 1
#define DEBUG_MSG(str)                fprintf(stderr, str)
#define DEBUG_PRINTF(fmt, args...)    fprintf(stderr,"%s:%d: "fmt, __FUNCTION__, __LINE__, args)
#define CHECK_NULL(a)                if(a==NULL){fprintf(stderr,"%s:%d: ERROR: NULL POINTER AS ARGUMENT\n", __FUNCTION__, __LINE__);return LGW_com_ERROR;}
#else
#define DEBUG_MSG(str)
#define DEBUG_PRINTF(fmt, args...)
#define CHECK_NULL(a)                if(a==NULL){return LGW_com_ERROR;}
#endif

/* -------------------------------------------------------------------------- */
/* --- PRIVATE SHARED VARIABLES (GLOBAL) ------------------------------------ */




															  /* -------------------------------------------------------------------------- */
															  /* --- PRIVATE CONSTANTS ---------------------------------------------------- */
/*
#define READ_ACCESS     0x00
#define WRITE_ACCESS    0x80
#define com_SPEED       8000000
#define com_DEV_PATH    "/dev/spidev0.0"
*/
															  //#define com_DEV_PATH    "/dev/spidev32766.0"

															  /* -------------------------------------------------------------------------- */
															  /* --- PUBLIC FUNCTIONS DEFINITION ------------------------------------------ */


															  /* configure TTYACM0 port*/


int set_interface_attribs(int fd, int speed, int parity)
{
	#ifdef _WIN32
		return set_interface_attribs_win(fd,speed,parity);
	#elif __linux__
		return set_interface_attribs_linux(fd, speed,parity);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif
}

void set_blocking(int fd, int should_block)
{
	#ifdef _WIN32
		return set_blocking_win(fd,should_block);
	#elif __linux__
		return set_blocking_linux(fd,should_block);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif
}




/* SPI initialization and configuration */
int lgw_com_open(void **com_target_ptr) {

	#ifdef _WIN32
		return lgw_com_open_win(com_target_ptr);
	#elif __linux__
		return lgw_com_open_linux(com_target_ptr);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif
}




/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

/* COM release */
int lgw_com_close(void *com_target) {
#ifdef _WIN32
		return lgw_com_close_win(com_target);
	#elif __linux__
		return lgw_com_close_linux(com_target);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

/* Simple write */
int lgw_com_w(void *com_target, uint8_t com_mux_mode, uint8_t com_mux_target, uint8_t address, uint8_t data) {
	#ifdef _WIN32
		return lgw_com_w_win(com_target,com_mux_mode,com_mux_target,address,data);
	#elif __linux__
		return lgw_com_w_linux(com_target,com_mux_mode,com_mux_target,address,data);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

/* Simple read */
int lgw_com_r(void *com_target, uint8_t com_mux_mode, uint8_t com_mux_target, uint8_t address, uint8_t *data) {
	#ifdef _WIN32
		return lgw_com_r_win(com_target,com_mux_mode,com_mux_target,address,data);
	#elif __linux__
		return lgw_com_r_linux(com_target,com_mux_mode,com_mux_target,address,data);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif

}


int lgw_com_wb(void *com_target, uint8_t com_mux_mode, uint8_t com_mux_target, uint8_t address, uint8_t *data, uint16_t size) {
#ifdef _WIN32
		return lgw_com_wb_win(com_target,com_mux_mode,com_mux_target,address,data,size);
	#elif __linux__
		return lgw_com_wb_linux(com_target,com_mux_mode,com_mux_target,address,data,size);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif	
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */


/* Burst (multiple-byte) read */
int lgw_com_rb(void *com_target, uint8_t com_mux_mode, uint8_t com_mux_target, uint8_t address, uint8_t *data, uint16_t size) {
#ifdef _WIN32
		return lgw_com_rb_win(com_target,com_mux_mode,com_mux_target,address,data,size);
	#elif __linux__
		return lgw_com_rb_linux(com_target,com_mux_mode,com_mux_target,address,data,size);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif		
}

int SendCmdn(CmdSettings_t CmdSettings, int file1)
{
    #ifdef _WIN32
		return SendCmdn_win(CmdSettings,file1);
	#elif __linux__
		return SendCmdn_linux(CmdSettings,file1);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif			
}


int ReceiveAns(AnsSettings_t *Ansbuffer, int file1)
{
    #ifdef _WIN32
		return ReceiveAns_win(Ansbuffer,file1);
	#elif __linux__
			return ReceiveAns_linux(Ansbuffer,file1);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif				
}


/*Embedded HAL into STM32 part */

int lgw_receive_cmd(void *com_target, uint8_t max_packet, uint8_t *data) {
	#ifdef _WIN32
		return lgw_receive_cmd_win(com_target,max_packet,data);
	#elif __linux__
		return lgw_receive_cmd_linux(com_target,max_packet,data);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif			
}



/*Embedded HAL into STM32 part */

int lgw_rxrf_setconfcmd(void *com_target, uint8_t rfchain, uint8_t *data, uint16_t size) {
	#ifdef _WIN32
		return lgw_rxrf_setconfcmd_win(com_target,rfchain,data,size);
	#elif __linux__
		return lgw_rxrf_setconfcmd_linux(com_target,rfchain,data,size);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif			
	
}

int lgw_boardconfcmd(void * com_target, uint8_t *data, uint16_t size)

{
	#ifdef _WIN32
		return  lgw_boardconfcmd_win(com_target,data,size);
	#elif __linux__
		return  lgw_boardconfcmd_linux(com_target,data,size);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif			

}
int lgw_rxif_setconfcmd(void *com_target, uint8_t ifchain, uint8_t *data, uint16_t size) {
	#ifdef _WIN32
		return lgw_rxif_setconfcmd_win(com_target,ifchain,data,size);
	#elif __linux__
		return lgw_rxif_setconfcmd_linux(com_target,ifchain,data,size);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif			
}

int lgw_txgain_setconfcmd(void *com_target, uint8_t *data, uint16_t size)
{
	#ifdef _WIN32
		return  lgw_txgain_setconfcmd_win(com_target,data,size);
	#elif __linux__
		return  lgw_txgain_setconfcmd_linux(com_target,data,size);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif			
}



int lgw_sendconfcmd(void *com_target, uint8_t *data, uint16_t size) {
	#ifdef _WIN32
		return lgw_sendconfcmd_win(com_target,data,size);
	#elif __linux__
		return lgw_sendconfcmd_linux(com_target,data,size);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif			
}



int lgw_trigger(void *com_target, uint8_t address, uint32_t *data) {
    #ifdef _WIN32
		return lgw_trigger_win(com_target,address,data);
	#elif __linux__
		return lgw_trigger_linux(com_target,address,data);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif				
}

int lgw_calibration_snapshot(void * com_target)

{
	#ifdef _WIN32
		return lgw_calibration_snapshot_win(com_target);
	#elif __linux__
		return lgw_calibration_snapshot_linux(com_target);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif				
}

int lgw_resetSTM32(void * com_target)
{
	#ifdef _WIN32
		return lgw_resetSTM32_win(com_target);
	#elif __linux__
		return lgw_resetSTM32_linux(com_target);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif			
}

int lgw_GOTODFU(void * com_target)
{
	#ifdef _WIN32
		return lgw_GOTODFU_win(com_target);
	#elif __linux__
			return lgw_GOTODFU_linux(com_target);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif				
}

int lgw_GetUniqueId(void * com_target,uint8_t * uid)
{
	#ifdef _WIN32
		return lgw_GetUniqueId_win(com_target,uid);
	#elif __linux__
		return lgw_GetUniqueId_linux(com_target,uid);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif			
}
 

/****************************/
int checkcmd(uint8_t cmd)
{
	#ifdef _WIN32
		return checkcmd_win(cmd);
	#elif __linux__
		return checkcmd_linux(cmd);
	#elif __APPLE__
		DEBUG_PRINTF("System is not recognized.");
	#elif __unix__
		DEBUG_PRINTF("System is not recognized.");
	#elif __posix__
		DEBUG_PRINTF("System is not recognized.");
	#else
		DEBUG_PRINTF("System is not recognized.");
	#endif		
}


/* --- EOF ------------------------------------------------------------------ */