/*
 * STM32 Controller for the ultimate 4 channel recorder
 * Copyright (C) 2012-2024 Adam Williams <broadcast at earthling dot net>
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 * 
 */



// This runs on the STM32, converting I2S to SPI for the raspberry pi
// accompanying board: zoom.pcb
// raspberry pi program: zoompi.c

// build with make zoom.bin;./uart_programmer zoom.bin
// Don't print on the serial port to reduce noise.



#include "linux.h"
#include "uart.h"
#include "misc.h"
#include "stm32f4xx.h"
#include "stm32f4xx_dac.h"
#include "stm32f4xx_dma.h"
#include "stm32f4xx_rcc.h"
#include "stm32f4xx_gpio.h"
#include "stm32f4xx_tim.h"
#include "stm32f4xx_flash.h"
#include "stm32f4xx_exti.h"
#include "stm32f4xx_spi.h"
#include "stm32f4xx_syscfg.h"
#include "zoom.h"

#define DEBUG_PIN (1 << 13)

// SPI buffers
// add spidev.bufsiz=32768 to /boot/cmdline.txt to expand this
#define SPI_TX_BUFSIZE 32768
#define SPI_RX_BUFSIZE 16
#define SPI_HEADER 8
#define SYNC_CODE 0xe5
uint8_t *rx_buffer;
// core coupled RAM is unavailable to the DMA
uint8_t *tx_buffer0;
uint8_t *tx_buffer1;
#define FRAME_SIZE 12
#define SPI_FRAMES ((SPI_TX_BUFSIZE - SPI_HEADER) / FRAME_SIZE)
// pointers to the transmit buffers
int current_tx_buffer = 0;
uint8_t *prev_buffer = 0;
uint8_t *next_buffer = 0;
// samples written into the DMA buffer for each ADC
int sample_counter0 = 0;
int sample_counter1 = 0;
// the I2S input
int i2s_state0 = 0;
int i2s_state1 = 0;
// the I2S output
int tx_state = 0;
// signed -0x80000000 - 0x7fffff00
// working buffers for receiving 16 bit segments off I2S
int32_t channel0 = 0;
int32_t channel1 = 0;
int32_t channel2 = 0;
int32_t channel3 = 0;
// working buffers for the monitoring samples
// -0x80000 - 0x7ffff
int32_t output0 = 0;
int32_t output1 = 0;
// the last complete samples
// -0x800000 - 0x7fffff
// I2S2/AUX
int32_t last_channel0 = 0;
int32_t last_channel1 = 0;
// I2S3/mane
int32_t last_channel2 = 0;
int32_t last_channel3 = 0;


int debug_counter = 0;



// settings
// 0 - 32
int monitor_volume0 = 0;
int monitor_volume1 = 0;

//int monitor_mode = MONITOR_2CH_DIFF;
int monitor_mode = MONITOR_1CH_DIFF;

// AK4524 stuff

#define AK4524_CS (1 << 14)
#define AK4524_CLK (1 << 11)
#define AK4524_DAT (1 << 2)
#define AK4524_DELAY 1

// write the config space
void write_ak4524(uint16_t reg, uint16_t value)
{
	uint16_t data = 0xa000 | (reg << 8) | value;

	CLEAR_PIN(GPIOB, AK4524_CLK);
	CLEAR_PIN(GPIOB, AK4524_CS);
	
	int i;
	for(i = 0; i < 16; i++)
	{
		if((data & 0x8000))
		{
			SET_PIN(GPIOB, AK4524_DAT);
		}
		else
		{
			CLEAR_PIN(GPIOB, AK4524_DAT);
		}
		
		udelay(AK4524_DELAY);
		SET_PIN(GPIOB, AK4524_CLK);
		udelay(AK4524_DELAY);
		CLEAR_PIN(GPIOB, AK4524_CLK);
		
		data <<= 1;
	}
	
	SET_PIN(GPIOB, AK4524_CS);
	udelay(AK4524_DELAY);
}


void init_ak4524()
{
    GPIO_InitTypeDef GPIO_InitStructure;
	GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
	GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
	GPIO_InitStructure.GPIO_Pin = AK4524_CS | AK4524_CLK | AK4524_DAT;
	GPIO_Init(GPIOB, &GPIO_InitStructure);
	SET_PIN(GPIOB, AK4524_CS);
	CLEAR_PIN(GPIOB, AK4524_CLK);
	CLEAR_PIN(GPIOB, AK4524_DAT);


// exit reset
	write_ak4524(1, 0x03);

// power mode, DAC mute off
	write_ak4524(0, 0x07);

// sample rate & I2S format
//	write_ak4524(2, 0x20); // Mode 1, 96khz
  	write_ak4524(2, 0x24); // Mode 1, 48khz

// L input gain
	write_ak4524(4, 0x7f);
// R input gain
	write_ak4524(5, 0x7f);


// DAC ATT level
	write_ak4524(6, 0xfe);
	write_ak4524(7, 0xfe);
}


void stop_ak4524()
{
// reset
	write_ak4524(1, 0x00);
	mdelay(100);
}




// the I2S
void init_i2s()
{
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI3, ENABLE);

// upper ADC
	GPIO_PinAFConfig(GPIOB, GPIO_PinSource15, GPIO_AF_SPI2);
	GPIO_PinAFConfig(GPIOB, GPIO_PinSource13, GPIO_AF_SPI2);
	GPIO_PinAFConfig(GPIOB, GPIO_PinSource12, GPIO_AF_SPI2);
    GPIO_InitTypeDef GPIO_InitStructure;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
	GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
	GPIO_InitStructure.GPIO_PuPd  = GPIO_PuPd_NOPULL;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_15 | GPIO_Pin_13 | GPIO_Pin_12;
	GPIO_Init(GPIOB, &GPIO_InitStructure);

	I2S_InitTypeDef I2S_InitStruct;
	SPI_I2S_DeInit(SPI2);
	I2S_InitStruct.I2S_Mode = I2S_Mode_SlaveRx;
	I2S_InitStruct.I2S_Standard = I2S_Standard_MSB;
	I2S_InitStruct.I2S_DataFormat = I2S_DataFormat_32b;
	I2S_InitStruct.I2S_MCLKOutput = I2S_MCLKOutput_Disable;
	I2S_InitStruct.I2S_AudioFreq = I2S_AudioFreq_96k;
	I2S_InitStruct.I2S_CPOL = I2S_CPOL_Low;
	I2S_Init(SPI2, &I2S_InitStruct);

	I2S_Cmd(SPI2, ENABLE);
	
// lower ADC
	GPIO_PinAFConfig(GPIOA, GPIO_PinSource15, GPIO_AF_SPI3);
	GPIO_PinAFConfig(GPIOC, GPIO_PinSource10, GPIO_AF_SPI3);
// page 57 C11 is GPIO_AF_SPI3  
//	GPIO_PinAFConfig(GPIOC, GPIO_PinSource11, GPIO_AF_SPI3);
// page 56 B4 is GPIO_AF_I2S3ext  
	GPIO_PinAFConfig(GPIOB, GPIO_PinSource4, GPIO_AF_I2S3ext);
	GPIO_PinAFConfig(GPIOC, GPIO_PinSource12, GPIO_AF_SPI3);
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_15;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10 | GPIO_Pin_12;
	GPIO_Init(GPIOC, &GPIO_InitStructure);

// C11 doesn't work, but B4 does
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_4;
	GPIO_Init(GPIOB, &GPIO_InitStructure);

// Some pins are pulldowns in the bootloader
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_2;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
	GPIO_Init(GPIOB, &GPIO_InitStructure);
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_4 | GPIO_Pin_5;
	GPIO_Init(GPIOC, &GPIO_InitStructure);
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2;
	GPIO_Init(GPIOD, &GPIO_InitStructure);

	
	SPI_I2S_DeInit(SPI3);
	
	I2S_Init(SPI3, &I2S_InitStruct);
// Make the I2S3ext_SD pin an output
	I2S_FullDuplexConfig(I2S3ext, &I2S_InitStruct);
	I2S_Cmd(SPI3, ENABLE);
	I2S_Cmd(I2S3ext, ENABLE);

//TRACE2
//print_hex(SPI3->I2SCFGR);
//print_hex(I2S3ext->I2SCFGR);
}

void reset_i2s()
{
// disable I2S
	I2S_Cmd(SPI2, DISABLE);
	I2S_Cmd(SPI3, DISABLE);
	I2S_Cmd(I2S3ext, DISABLE);

// stop the ADC.  Have to reset them both.
	stop_ak4524();


// reset the I2S states
	i2s_state0 = 0;
	i2s_state1 = 0;
	tx_state = 0;

// enable I2S		
	I2S_Cmd(SPI2, ENABLE);
	I2S_Cmd(SPI3, ENABLE);
	I2S_Cmd(I2S3ext, ENABLE);

// restart the ADC
	init_ak4524();
}



#define DMA_RX_STREAM DMA2_Stream2
#define DMA_TX_STREAM DMA2_Stream5

DMA_InitTypeDef DMA_InitStructure;
// SPI cycle is too slow to do in a single iteration
void (*spi_init_state)();

void spi_init5()
{
	DMA_Cmd(DMA_RX_STREAM, ENABLE);
	DMA_Cmd(DMA_TX_STREAM, ENABLE);
	spi_init_state = 0;
}



void spi_init4()
{

// send off the current buffer
	DMA_InitStructure.DMA_Memory0BaseAddr = (uint32_t)prev_buffer;
	DMA_InitStructure.DMA_DIR = DMA_DIR_MemoryToPeripheral;
	DMA_InitStructure.DMA_BufferSize = SPI_TX_BUFSIZE;
	DMA_Init(DMA_TX_STREAM, &DMA_InitStructure);  	
	spi_init_state = spi_init5;
}


void spi_init3()
{

// enable the transmit DMA
// this must be done in a single function

	if(current_tx_buffer == 0)
	{
		prev_buffer = tx_buffer0;
		next_buffer = tx_buffer1;
	}
	else
	{
		prev_buffer = tx_buffer1;
		next_buffer = tx_buffer0;
	}
	
	current_tx_buffer = !current_tx_buffer;

	next_buffer[0] = 0xe6;
	next_buffer[1] = 0;
	next_buffer[2] = 0;
	next_buffer[3] = 0;
// reset the sample counters
	*(uint16_t*)(next_buffer + 4) = 0;
	*(uint16_t*)(next_buffer + 6) = 0;
	sample_counter0 = 0;
	sample_counter1 = 0;
	
	
	spi_init_state = spi_init4;
}

void spi_init2()
{
// enable receive DMA
  	DMA_InitStructure.DMA_Channel = DMA_Channel_3;  
  	DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&SPI1->DR;
	DMA_InitStructure.DMA_Memory0BaseAddr = (uint32_t)rx_buffer;
	DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralToMemory;
	DMA_InitStructure.DMA_BufferSize = SPI_RX_BUFSIZE;
	DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
	DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
	DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
	DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte; 
	DMA_InitStructure.DMA_Mode = DMA_Mode_Normal;
	DMA_InitStructure.DMA_Priority = DMA_Priority_High;
  	DMA_InitStructure.DMA_FIFOMode = DMA_FIFOMode_Enable;         
  	DMA_InitStructure.DMA_FIFOThreshold = DMA_FIFOThreshold_Full;
  	DMA_InitStructure.DMA_MemoryBurst = DMA_MemoryBurst_Single;
  	DMA_InitStructure.DMA_PeripheralBurst = DMA_PeripheralBurst_Single;  
  	DMA_Init(DMA_RX_STREAM, &DMA_InitStructure);
	spi_init_state = spi_init3;
}

void spi_init1()
{

	DMA_DeInit(DMA_RX_STREAM);
	DMA_DeInit(DMA_TX_STREAM);

	spi_init_state = spi_init2;
}



void start_spi_cycle()
{
//TOGGLE_PIN(GPIOA, DEBUG_PIN);
	spi_init_state = spi_init1;
}




void init_spi()
{
  	GPIO_InitTypeDef GPIO_InitStructure;
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA2, ENABLE);
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE);


// SS
	GPIO_PinAFConfig(GPIOA, GPIO_PinSource4, GPIO_AF_SPI1);
// SCK
	GPIO_PinAFConfig(GPIOA, GPIO_PinSource5, GPIO_AF_SPI1);
// MISO
	GPIO_PinAFConfig(GPIOA, GPIO_PinSource6, GPIO_AF_SPI1);
// MOSI
	GPIO_PinAFConfig(GPIOA, GPIO_PinSource7, GPIO_AF_SPI1);

	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
	GPIO_InitStructure.GPIO_PuPd  = GPIO_PuPd_DOWN;

	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_4 | GPIO_Pin_5 | GPIO_Pin_6 | GPIO_Pin_7;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	SPI_I2S_DeInit(SPI1);
	
	SPI_InitTypeDef SPI_InitStructure;
	SPI_InitStructure.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
	SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;
	SPI_InitStructure.SPI_CPOL = SPI_CPOL_Low;
	SPI_InitStructure.SPI_CPHA = SPI_CPHA_1Edge;
	SPI_InitStructure.SPI_NSS = SPI_NSS_Hard;
	SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_16;
	SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;
	SPI_InitStructure.SPI_CRCPolynomial = 7;

	SPI_InitStructure.SPI_Mode = SPI_Mode_Slave;
	SPI_InitStructure.SPI_NSS = SPI_NSS_Hard;
	SPI_Init(SPI1, &SPI_InitStructure);

	start_spi_cycle();
	while(spi_init_state != 0)
	{
		spi_init_state();
	}

	SPI_Cmd(SPI1, ENABLE);

	SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Rx, ENABLE);
	SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Tx, ENABLE);
}




#define ADC_STATE_MACHINE(got_adc, state, channel0, channel1) \
switch(state) \
{ \
	case 0: \
		if(!side) \
		{ \
			channel0 = value << 16; \
			state++; \
		} \
		break; \
 \
	case 1: \
		if(!side) \
		{ \
			channel0 |= value; \
			state++; \
		} \
		else \
		{ \
			state = 0; \
		} \
		break; \
 \
	case 2: \
		if(side) \
		{ \
			channel1 = value << 16; \
			state++; \
		} \
		else \
		{ \
			state = 0; \
		} \
		break; \
 \
	case 3: \
		if(side) \
		{ \
			channel1 |= value; \
			got_adc = 1; \
			channel0 >>= 8; \
			channel1 >>= 8; \
	 	} \
 \
		state = 0; \
		break; \
}



int main(void)
{
	init_linux();
// more can be allocated using linux.c:kmalloc than statically
// TODO: define the heap size for each program separately
	rx_buffer = kmalloc(SPI_RX_BUFSIZE, 0);
	tx_buffer0 = kmalloc(SPI_TX_BUFSIZE, 0);
	tx_buffer1 = kmalloc(SPI_TX_BUFSIZE, 0);


/* Enable the GPIOs */
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA |
			RCC_AHB1Periph_GPIOB |
			RCC_AHB1Periph_GPIOC |
			RCC_AHB1Periph_GPIOD |
			RCC_AHB1Periph_GPIOE |
			RCC_AHB1Periph_CCMDATARAMEN, 
		ENABLE);

// enable the interrupt handler
 	NVIC_SetVectorTable(NVIC_VectTab_FLASH, PROGRAM_START - 0x08000000);

	init_uart();
	
	print_text("Welcome to the ultimate audio recorder\n");
	flush_uart();


    GPIO_InitTypeDef GPIO_InitStructure;
	GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
	GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
	GPIO_InitStructure.GPIO_Pin = DEBUG_PIN;
	GPIO_Init(GPIOA, &GPIO_InitStructure);
	SET_PIN(GPIOA, DEBUG_PIN)
//	CLEAR_PIN(GPIOA, DEBUG_PIN)

// I2S must be initialized first to align the samples
	init_i2s();

	init_spi();


// must be initialized last
	init_ak4524();

	while(1)
	{
		handle_uart();
		if(spi_init_state != 0)
		{
			spi_init_state();
		}

		if(spi_init_state == 0 &&
			(uint16_t)DMA_TX_STREAM->NDTR <= 0)
		{

// static int debug_counter = 0;
// debug_counter++;
// if(debug_counter > 100)
// {
// TRACE2
// print_hex2(rx_buffer[0]);
// print_hex2(rx_buffer[1]);
// print_hex2(rx_buffer[2]);
// print_hex2(rx_buffer[3]);
// print_hex2(rx_buffer[4]);
// print_hex2(rx_buffer[5]);
// print_hex2(rx_buffer[6]);
// print_hex2(rx_buffer[7]);
// 
// debug_counter = 0;
// }

// search for the start code
// off by 0-1 byte.  Don't know why
			int i;
			for(i = 0; i < 8; i++)
			{
				if(rx_buffer[i] == SYNC_CODE)
				{
					monitor_volume0 = rx_buffer[i + 1];
					monitor_volume1 = rx_buffer[i + 2];
					monitor_mode = rx_buffer[i + 3];
					break;
				}
			}

// switch TX buffers
 			start_spi_cycle();
		}



// framing error
		if((SPI2->SR & SPI_I2S_FLAG_TIFRFE) ||
			(SPI3->SR & SPI_I2S_FLAG_TIFRFE))
		{
			TRACE2
			print_text("FRAMING ERROR");

			reset_i2s();
		}
	
		int got_adc0 = 0, got_adc1 = 0;

// handle the aux I2S
// 2 interrupts are fired for each side
		if((SPI2->SR & SPI_I2S_FLAG_RXNE))
		{
			uint32_t value = SPI2->DR;
			int side = (SPI2->SR & I2S_FLAG_CHSIDE);
			

			ADC_STATE_MACHINE(got_adc0, i2s_state0, channel0, channel1)
			
			
			
// store in the DMA buffer
			if(got_adc0)
			{
				if(sample_counter0 < SPI_FRAMES)
				{
					int offset = SPI_HEADER + sample_counter0 * FRAME_SIZE;
 					next_buffer[offset + 0] = channel0 & 0xff;
 					next_buffer[offset + 1] = (channel0 >> 8) & 0xff;
 					next_buffer[offset + 2] = (channel0 >> 16) & 0xff;
 					next_buffer[offset + 3] = channel1 & 0xff;
 					next_buffer[offset + 4] = (channel1 >> 8) & 0xff;
 					next_buffer[offset + 5] = (channel1 >> 16) & 0xff;
					sample_counter0++;
					*(uint16_t*)(next_buffer + 4) = sample_counter0;
				}
				
				
				last_channel0 = channel0;
				last_channel1 = channel1;
			}
			
		}
		
		
// handle the mane I2S
		if((SPI3->SR & SPI_I2S_FLAG_RXNE))
		{
			uint32_t value = SPI3->DR;
			int side = (SPI3->SR & I2S_FLAG_CHSIDE);
			
			ADC_STATE_MACHINE(got_adc1, i2s_state1, channel2, channel3)

			if(got_adc1)
			{
				if(sample_counter1 < SPI_FRAMES)
				{
					int offset = SPI_HEADER + sample_counter1 * FRAME_SIZE + 6;


// DEBUG
//channel2 = (debug_counter % 109) * 0xffffff / 109 - 0x800000;
//channel3 = 0x7fffff;

 					next_buffer[offset + 0] = channel2 & 0xff;
 					next_buffer[offset + 1] = (channel2 >> 8) & 0xff;
 					next_buffer[offset + 2] = (channel2 >> 16) & 0xff;
 					next_buffer[offset + 3] = channel3 & 0xff;
 					next_buffer[offset + 4] = (channel3 >> 8) & 0xff;
 					next_buffer[offset + 5] = (channel3 >> 16) & 0xff;
					sample_counter1++;
					*(uint16_t*)(next_buffer + 6) = sample_counter1;
				}

				last_channel2 = channel2;
				last_channel3 = channel3;

// combine the 4 samples into the next 2 monitoring samples
			}
		}


// handle the mane I2S output
		if((I2S3ext->SR & SPI_I2S_FLAG_TXE))
		{
			int side = (I2S3ext->SR & I2S_FLAG_CHSIDE);
			
			switch(tx_state)
			{
				case 0:
					if(!side)
					{
						I2S3ext->DR = output0 >> 16;
						tx_state++;
					}
					break;
				case 1:
					if(!side)
					{
						I2S3ext->DR = output0 & 0xffff;
						tx_state++;
					}
					else
					{
						tx_state = 0;
					}
					break;
				case 2:
					if(side)
					{
						I2S3ext->DR = output1 >> 16;
						tx_state++;
					}
					else
					{
						tx_state = 0;
					}
					break;
				case 3:
					if(side)
					{
						I2S3ext->DR = output1 & 0xffff;


						



					}
					tx_state = 0;

// test waveform
// debug_counter++;
// int value = (debug_counter % 109) * 0xffffff / 109 - 0x800000;
// last_channel0 = value;
// last_channel1 = value;
// last_channel2 = value;
// last_channel3 = value;


// downmix the next sample
					switch(monitor_mode)
					{
						case MONITOR_2CH_DIFF:
							output0 = last_channel0 - last_channel1;
							output0 = output0 * monitor_volume0 / 2;
							output1 = last_channel2 - last_channel3;
							output1 = output1 * monitor_volume1 / 2;
							break;
						
						case MONITOR_4CH:
							output0 = last_channel0 * monitor_volume0 + 
								last_channel2 * monitor_volume1;
							output1 = last_channel1 * monitor_volume0 + 
								last_channel3 * monitor_volume1;
							break;
						
						case MONITOR_2CH:
							output0 = last_channel2 * monitor_volume1;
							output1 = last_channel3 * monitor_volume1;
							break;
						
						case MONITOR_3CH:
						{
							int temp = (last_channel2 - last_channel3) / 2;
							output0 = last_channel0 * monitor_volume0 + 
								temp * monitor_volume1;
							output1 = last_channel1 * monitor_volume0 + 
								temp * monitor_volume1;
							break;
						}
						
						case MONITOR_2CH_AVG:
							output0 = last_channel0 + last_channel1;
							output1 = last_channel2 + last_channel3;
							output0 = output0 * monitor_volume0 / 2;
							output1 = output1 * monitor_volume1 / 2;
							break;
							
						default:
						case MONITOR_1CH_DIFF:
							output0 = last_channel2 - last_channel3;
							output0 = output0 * monitor_volume1 / 2;
							output1 = output0;
							break;
					}

// convert to 20 bits
// right shift 5: volume
// right shift 4: 24 bits to 20 bits
					output0 = output0 >> 9;
					output1 = output1 >> 9;

// shift & scale to fit in 3.3V
					output0 = (output0 - 0x1b6db) * 80 / 100;
					output1 = (output1 - 0x1b6db) * 80 / 100;



// clamp
					if(output0 > 0x7ffff)
					{
						output0 = 0x7ffff;
					}
					if(output1 > 0x7ffff)
					{
						output1 = 0x7ffff;
					}
					if(output0 < -0x80000)
					{
						output0 = -0x80000;
					}
					if(output1 < -0x80000)
					{
						output1 = -0x80000;
					}



// output test waveform
// debug_counter++;
// if((debug_counter % 109) > 54)
// {
// 	output0 = 0x7ffff;
// 	output1 = 0x7ffff;
// }
// else
// {
// 	output0 = 0x80000;
// 	output1 = 0x80000;
// }

					break;
			}
			
			
		}
		
	}
	
	
}
















