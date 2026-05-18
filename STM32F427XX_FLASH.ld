/*
******************************************************************************
**

**  File        : LinkerScript.ld
**
**  Author      : STM32CubeMX
**
**  Abstract    : Linker script for STM32F427VGTx series
**                1024Kbytes FLASH and 256Kbytes RAM
**
**                RAM    : 192K at 0x20000000
**                CCMRAM : 64K  at 0x10000000
**                FLASH  : 1024K at 0x08000000
**
**  Target      : STMicroelectronics STM32
**
*****************************************************************************
*/

/* Entry Point */
ENTRY(Reset_Handler)

/* Highest address of the user mode stack */
_estack = ORIGIN(RAM) + LENGTH(RAM);    /* end of RAM */

/* Generate a link error if heap and stack don't fit into RAM */
_Min_Heap_Size = 0x200;     /* required amount of heap  */
_Min_Stack_Size = 0x400;    /* required amount of stack */

/* Specify the memory areas */
MEMORY
{
  RAM    (xrw) : ORIGIN = 0x20000000, LENGTH = 192K
  CCMRAM (xrw) : ORIGIN = 0x10000000, LENGTH = 64K
  FLASH  (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
}

/* Define output sections */
SECTIONS
{
  /* The startup code goes first into FLASH */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))    /* Startup code */
    . = ALIGN(4);
  } >FLASH

  /* The program code and other data goes into FLASH */
  .text :
  {
    . = ALIGN(4);

    *(.text)
    *(.text*)

    *(.glue_7)
    *(.glue_7t)

    *(.eh_frame)

    KEEP(*(.init))
    KEEP(*(.fini))

    . = ALIGN(4);
    _etext = .;
  } >FLASH

  /* Constant data goes into FLASH */
  .rodata :
  {
    . = ALIGN(4);

    *(.rodata)
    *(.rodata*)

    . = ALIGN(4);
  } >FLASH

  /*
   * Exception unwind tables.
   * 注意：
   * 这里不能写成 ".ARM.extab (READONLY) :"
   * arm-none-eabi-gcc 9.3.1 / 老版本 ld 不兼容 READONLY 关键字。
   */
  .ARM.extab :
  {
    . = ALIGN(4);
    *(.ARM.extab* .gnu.linkonce.armextab.*)
    . = ALIGN(4);
  } >FLASH

  .ARM :
  {
    . = ALIGN(4);
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
    . = ALIGN(4);
  } >FLASH

  .preinit_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array*))
    PROVIDE_HIDDEN(__preinit_array_end = .);
    . = ALIGN(4);
  } >FLASH

  .init_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    PROVIDE_HIDDEN(__init_array_end = .);
    . = ALIGN(4);
  } >FLASH

  .fini_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array*))
    PROVIDE_HIDDEN(__fini_array_end = .);
    . = ALIGN(4);
  } >FLASH

  /* Used by the startup to initialize data */
  _sidata = LOADADDR(.data);

  /* Initialized data sections goes into RAM, load LMA copy after code */
  .data :
  {
    . = ALIGN(4);

    _sdata = .;

    *(.data)
    *(.data*)

    *(.RamFunc)
    *(.RamFunc*)

    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

  /*
   * CCMRAM section
   *
   * 注意：
   * 如果你把“有初始值的变量”放进 .ccmram，
   * 启动文件 startup_stm32f427xx.s 需要额外拷贝初始化数据。
   *
   * 如果只是放 DMA 不访问的高速缓存、缓冲区、算法临时数组，
   * 建议放未初始化变量，例如：
   * __attribute__((section(".ccmram"))) uint8_t buffer[1024];
   */
  _siccmram = LOADADDR(.ccmram);

  .ccmram :
  {
    . = ALIGN(4);

    _sccmram = .;

    *(.ccmram)
    *(.ccmram*)

    . = ALIGN(4);
    _eccmram = .;
  } >CCMRAM AT> FLASH

  /* Uninitialized data section */
  . = ALIGN(4);

  .bss :
  {
    _sbss = .;
    __bss_start__ = _sbss;

    *(.bss)
    *(.bss*)
    *(COMMON)

    . = ALIGN(4);

    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  /*
   * User_heap_stack section, used to check that there is enough RAM left
   */
  ._user_heap_stack :
  {
    . = ALIGN(8);

    PROVIDE(end = .);
    PROVIDE(_end = .);

    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;

    . = ALIGN(8);
  } >RAM

  /*
   * Remove information from the standard libraries
   */
  /DISCARD/ :
  {
    libc.a  ( * )
    libm.a  ( * )
    libgcc.a( * )
  }
}