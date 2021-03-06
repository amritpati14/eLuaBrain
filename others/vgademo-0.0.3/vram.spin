{{
Video RAM engine
Uses the SPI engine by Beau Schwabe, optimized for speed
}}

CON
    '' Pins
    RAM_CS_PIN              = 0
    RAM_CS_MASK             = 1 << RAM_CS_PIN    
    RAM_MISO_PIN            = 1
    RAM_MISO_MASK           = 1 << RAM_MISO_PIN    
    RAM_MOSI_PIN            = 2
    RAM_MOSI_MASK           = 1 << RAM_MOSI_PIN    
    RAM_CLK_PIN             = 3
    RAM_CLK_MASK            = 1 << RAM_CLK_PIN    
    VBUF_PIN                = 4
    VBUF_MSK                = 1 << VBUF_PIN    
    HOST_WR_PIN             = 5
    HOST_WR_MASK            = 1 << HOST_WR_PIN
    TEMP_PIN                = 6
    TEMP_PIN_MASK           = 1 << TEMP_PIN

    SRAM_READ_CMD           = 3
    SRAM_WRITE_CMD          = 2
    SRAM_WRSTATUS_CMD       = 1
    SRAM_STATUS             = %01000001
    VMEM_SIZE_LONGS         = (80 * 30 * 2) / 4
    VMEM_SIZE_CHARS         = (80 * 30)
    CURSOR_DEFAULT_MODE     = 2
    CURSOR_DATA_SIZE        = 1

VAR
    long     cog    
    
'------------------------------------------------------------------------------------------------------------------------------
PUB start(SyncPtr, ScreenPtr, ScreenPtr2, CursorPtr) : okay
    stop
    screen_base := ScreenPtr
    screen_base2 := ScreenPtr2
    cursor_ptr := CursorPtr
    'longmove(@screen_base, @ScreenPtr, 2)
    'longmove(@screen_base2, @ScreenPtr2, 2)
    okay := cog := cognew(@main, SyncPtr) + 1
    
PUB stop
'' Stop SPI Engine - frees a cog
    if cog
       cogstop(cog~ - 1)
    
'################################################################################################################
DAT           org
'  
'' SPI Engine - main loop
'
main
              ' One time SRAM initialization: setup sequential mode
              mov     outa,           #%0001
              mov     dira,           #%1101        wz
              muxz    outa,           #RAM_CS_MASK
              mov     data,           #SRAM_WRSTATUS_CMD
              call    #sram_wr
              mov     data,           #SRAM_STATUS
              call    #sram_wr
              or      outa,           #RAM_CS_MASK  wz
              nop
              muxz    outa,           #RAM_CS_MASK
              
              ' Clear memory                         
              ' Send sequential write command to SRAM
              mov     data,           #SRAM_WRITE_CMD              
              call    #sram_wr
              mov     data,           #0
              call    #sram_wr
              call    #sram_wr
              ' Write cursor data first
              call     #sram_wr
              call     #sram_wr
              mov      data,          #CURSOR_DEFAULT_MODE
              call     #sram_wr
              mov      data,          #0
              call     #sram_wr              

              ' Then clear the memory
              mov     datacnt,        vmem_chars
cloop
              mov     data,           #32
              call    #sram_wr
              mov     data,           #7
              call    #sram_wr
              djnz    datacnt,        #cloop                                                
              
              ' Actual setup of pin directions
              mov     outa,           #%0100000
              mov     dira,           #%1110000
              mov     vbuf_mask,      #VBUF_MSK
              mov     miso_mask,      #RAM_MISO_MASK
              
loop          ' Wait for vertical sync
              rdlong  t0,             par           wz
        if_z  jmp     #loop                         
        
              ' Got vertical sync indication from vgacolour, so change VBUF pin to signal buffer change
              ' and set cursor data
              xor     outa,           vbuf_mask
              wrlong  cursor_data,    cursor_ptr
              or      outa,           #TEMP_PIN_MASK
              
              ' Get destination buffer address
              test    vbuf_mask,      ina           wz
        if_z  mov     dataptr,        screen_base2
       if_nz  mov     dataptr,        screen_base              

              ' Take control over the SPI lines CS, MOSI and CLK, enable CS
              or      dira,           #(RAM_CS_MASK | RAM_MOSI_MASK | RAM_CLK_MASK)  wz   ' z = 0
              muxz    outa,           #RAM_CS_MASK
              
              ' Send 'read' command followed by two zeroes to the SRAM to initiate sequential reading
              mov     data,           #SRAM_READ_CMD
              call    #sram_wr
              mov     data,           #0
              call    #sram_wr              
              call    #sram_wr

              ' Start copying the data from the serial SRAM to dataptr
              mov     datacnt,        vmem_size
              add     datacnt,        #CURSOR_DATA_SIZE
              mov     cursor_data,    #0            wz
cploop        call    #sram_rd
              mov     longdata,       data
              call    #sram_rd
              shl     data,           #8
              or      longdata,       data
              call    #sram_rd
              shl     data,           #16
              or      longdata,       data
              call    #sram_rd
              shl     data,           #24
              or      longdata,       data
        if_z  mov     cursor_data,    longdata                         
       if_nz  wrlong  longdata,       dataptr
       if_nz  add     dataptr,        #4
              djnz    datacnt,        #cploop       wz                         

              ' Done copying: reset CS, MOSI and CLK to inputs and signal the host that it may
              ' write data to the SRAM (z = 1 here because of the previous djnz)
              muxnz   dira,           #(RAM_CS_MASK | RAM_MOSI_MASK | RAM_CLK_MASK)
              muxz    outa,           #RAM_CS_MASK
              muxnz   outa,           #HOST_WR_MASK

              ' Leave the WR pin on for 1us, then reset it to 0 and restart the loop
              mov     t0,             cnt
              add     t0,             #80
              waitcnt t0,             #0  
              or      outa,           #HOST_WR_MASK

              ' All done, clear sync indication now
              wrlong  zero,           par
              muxnz    outa,          #TEMP_PIN_MASK
                            
              jmp     #loop
               
'################################################################################################################

sram_wr
              mov     t0,             #(1 << 7)         ''          Create MSB mask
MSB_Sout      test    data,           t0      wc        ''          Test MSB of DataValue
              muxc    outa,           #RAM_MOSI_MASK    ''          Set DataBit HIGH or LOW
              shr     t0,             #1      wz        ''          Prepare for next DataBit
              xor     outa,           #RAM_CLK_MASK      
              xor     outa,           #RAM_CLK_MASK                                      
      if_nz   jmp     #MSB_Sout
sram_wr_ret   ret         
              
'------------------------------------------------------------------------------------------------------------------------------

sram_rd
              mov     data,           #0
              ' Unroll loop to maximize speed
MSBPRE_Sin    ' 8 bits      
              test    miso_mask,      ina     wc        ''          Read Data Bit into 'C' flag
              rcl     data,           #1                ''          rotate "C" flag into return value
              xor     outa,           #RAM_CLK_MASK 
              xor     outa,           #RAM_CLK_MASK
              test    miso_mask,      ina     wc        ''          Read Data Bit into 'C' flag
              rcl     data,           #1                ''          rotate "C" flag into return value
              xor     outa,           #RAM_CLK_MASK
              xor     outa,           #RAM_CLK_MASK                          
              test    miso_mask,      ina     wc        ''          Read Data Bit into 'C' flag
              rcl     data,           #1                ''          rotate "C" flag into return value
              xor     outa,           #RAM_CLK_MASK
              xor     outa,           #RAM_CLK_MASK                          
              test    miso_mask,      ina     wc        ''          Read Data Bit into 'C' flag
              rcl     data,           #1                ''          rotate "C" flag into return value
              xor     outa,           #RAM_CLK_MASK
              xor     outa,           #RAM_CLK_MASK                          
              test    miso_mask,      ina     wc        ''          Read Data Bit into 'C' flag
              rcl     data,           #1                ''          rotate "C" flag into return value
              xor     outa,           #RAM_CLK_MASK
              xor     outa,           #RAM_CLK_MASK                          
              test    miso_mask,      ina     wc        ''          Read Data Bit into 'C' flag
              rcl     data,           #1                ''          rotate "C" flag into return value
              xor     outa,           #RAM_CLK_MASK
              xor     outa,           #RAM_CLK_MASK                          
              test    miso_mask,      ina     wc        ''          Read Data Bit into 'C' flag
              rcl     data,           #1                ''          rotate "C" flag into return value
              xor     outa,           #RAM_CLK_MASK
              xor     outa,           #RAM_CLK_MASK                          
              test    miso_mask,      ina     wc        ''          Read Data Bit into 'C' flag
              rcl     data,           #1                ''          rotate "C" flag into return value
              xor     outa,           #RAM_CLK_MASK
              xor     outa,           #RAM_CLK_MASK                      
sram_rd_ret   ret

{
########################### Assembly variables ###########################
}
zero                  long            0

vbuf_mask             long            0
miso_mask             long            0
data                  long            0
longdata              long            0
datacnt               long            0
vmem_size             long            (VMEM_SIZE_LONGS)
vmem_chars            long            (VMEM_SIZE_CHARS)

' Temps
t0                    long            0

screen_base           long            0
screen_base2          long            0
cursor_ptr            long            0
cursor_data           long            0
dataptr               long            0

{{
┌──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                                   TERMS OF USE: MIT License                                                  │                                                            
├──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation    │ 
│files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,    │
│modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software│
│is furnished to do so, subject to the following conditions:                                                                   │
│                                                                                                                              │
│The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.│
│                                                                                                                              │
│THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE          │
│WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR         │
│COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,   │
│ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                         │
└──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
}}