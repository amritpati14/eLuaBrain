// nRF functions

#include "type.h"
#include "nrf.h"
#include "nrf_ll.h"
#include "nrf_conf.h"
#include <stdio.h>
#include <string.h>

// ****************************************************************************
// Local variables and definitions

#ifdef NRF_CFG_PROFILE_SERVER
static u8 nrf_p0_addr[] = NRF_CFG_SRV_PIPE0_ADDR;
static const u8 nrf_srv_p1_addr[] = NRF_CFG_SRV_PIPE1_ADDR;
#else // #ifdef NRF_CFG_PROFILE_SERVER
static u8 nrf_p0_addr[] = NRF_CLIENT_ADDR_POOL;
#endif // #ifdef NRF_CFG_PROFILE_SERVER
static u8 nrf_crt_mode = 0xFF;
static u32 nrf_lfsr = 1;

#ifdef NRF_DEBUG
#define nrfprint              printf
#else
void nrfprint( const char *fmt, ... )
{
}
#endif

#define NRFMIN( x, y )        ( ( x ) < ( y ) ? ( x ) : ( y ) )

// ****************************************************************************
// Helpers

static void nrfh_ll_send_byte( u8 data )
{
  nrf_ll_send_packet( &data, 1 );
}

static void nrfh_generic_read( u8 cmd, u8 *dataptr, u16 len )
{
  nrf_ll_csn_low();
  nrfh_ll_send_byte( cmd );
  nrf_ll_flush( 1 );
  nrf_ll_read_packet( dataptr, len );
  nrf_ll_csn_high();
}

static void nrfh_generic_write( u8 cmd, const u8  *dataptr, u16 len )
{
  nrf_ll_csn_low();
  nrfh_ll_send_byte( cmd );
  if( len > 0 )
    nrf_ll_send_packet( dataptr, len );
  nrf_ll_flush( len + 1 );    
  nrf_ll_csn_high();
}

// LFSR based pseudo RNG from http://en.wikipedia.org/wiki/Linear_feedback_shift_register
static u32 nrfh_rand()
{
  nrf_lfsr = ( nrf_lfsr >> 1 ) ^ ( -( nrf_lfsr & 1u ) & 0xD0000001u ); 
  return nrf_lfsr;
}

// ****************************************************************************
// Basic nRF commands

void nrf_read_register( u8 address, u8 *dataptr, u16 len )
{
  nrfh_generic_read( NRF_CMD_R_REGISTER | address, dataptr, len );
}

u8 nrf_read_register_byte( u8 address )
{
  u8 data;

  nrf_read_register( address, &data, 1 );
  return data;
}

void nrf_write_register( u8 address, const u8 *dataptr, u16 len )
{
  nrfh_generic_write( NRF_CMD_W_REGISTER | address, dataptr, len );
}

void nrf_write_register_byte( u8 address, u8 data )
{
  nrf_write_register( address, &data, 1 );
}

void nrf_get_rx_payload( u8 *dataptr, u16 len )
{
  nrfh_generic_read( NRF_CMD_R_RX_PAYLOAD, dataptr, len );
}

void nrf_write_tx_payload( const u8 *dataptr, u16 len )
{
  nrfh_generic_write( NRF_CMD_W_TX_PAYLOAD, dataptr, len );
}

void nrf_flush_rx()
{
  nrfh_generic_write( NRF_CMD_FLUSH_RX, NULL, 0 );
}

void nrf_flush_tx()
{
  nrfh_generic_write( NRF_CMD_FLUSH_TX, NULL, 0 );
}

u8 nrf_get_status()
{
  return nrf_read_register_byte( NRF_REG_STATUS );
}

// ****************************************************************************
// Higher level nRF commands

void nrf_set_setup_retr( unsigned delay, unsigned count )
{
  nrf_write_register_byte( NRF_REG_SETUP_RETR, ( delay << 4 ) | count );
}

void nrf_set_rf_setup( int data_rate, int pwr, int lna )
{
  nrf_write_register_byte( NRF_REG_RF_SETUP, ( data_rate << 3 ) | ( pwr << 1 ) | lna );
}

void nrf_set_rx_addr( int pipe, const u8* paddr )
{
  nrf_write_register( NRF_REG_RX_ADDR( pipe ), paddr, 5 ); 
}

void nrf_get_rx_addr( int pipe, u8 *addrbuf )
{
  nrf_read_register( NRF_REG_RX_ADDR( pipe ), addrbuf, 5 );
}

void nrf_set_tx_addr( const u8 *paddr )
{
  nrf_write_register( NRF_REG_TX_ADDR, paddr, 5 );
}

void nrf_set_payload_size( int pipe, u8 size )
{
  nrf_write_register_byte( NRF_REG_RX_PW( pipe ), size );
}

void nrf_set_mode( int mode )
{
  if( mode == nrf_crt_mode )
    return;
  nrf_write_register_byte( NRF_REG_CONFIG, ( nrf_read_register_byte( NRF_REG_CONFIG ) & 0xFE ) | mode );
  nrf_ll_set_ce( mode == NRF_MODE_RX ? 1 : 0 );
  nrf_set_rx_addr( 0, nrf_p0_addr );
  nrf_crt_mode = ( u8 )mode;
}

void nrf_clear_interrupt( u8 mask )
{
  nrf_write_register_byte( NRF_REG_STATUS, mask );
}

int nrf_has_data()
{
  return ( nrf_read_register_byte( NRF_REG_STATUS ) & NRF_INT_RX_DR_MASK ) != 0;
}

unsigned nrf_send_packet( const u8 *addr, const u8 *pdata, unsigned len )
{
  static u8 txdata[ NRF_PAYLOAD_SIZE ];
  u8 stat;

  nrf_set_tx_addr( addr );
  nrf_set_rx_addr( 0, addr );
  memset( txdata, 0, NRF_PAYLOAD_SIZE );
  memcpy( txdata, pdata, NRFMIN( len, NRF_PAYLOAD_SIZE ) );
  nrf_write_tx_payload( txdata, NRF_PAYLOAD_SIZE );
  nrf_ll_ce_high();
  nrf_ll_delay_us( 10 );
  nrf_ll_ce_low();
  // [TODO] change this when interrupts will be used
  while( 1 )
  {
    stat = nrf_get_status();
    if( stat & NRF_INT_MAX_RT_MASK )
    {
      nrf_clear_interrupt( NRF_INT_MAX_RT_MASK );
      nrf_flush_tx();
      len = 0;
      break;
    }
    if( stat & NRF_INT_TX_DS_MASK )
    {
      nrf_clear_interrupt( NRF_INT_TX_DS_MASK );
      break;
    }
  }
  return len;
}

unsigned nrf_get_packet( u8 *pdata, unsigned maxlen, int *pipeno )
{
  u8 stat;
  static u8 rxdata[ NRF_PAYLOAD_SIZE ];

  maxlen = NRFMIN( maxlen, NRF_PAYLOAD_SIZE );
  stat = nrf_get_status();
  if( ( stat & NRF_INT_RX_DR_MASK ) == 0 )
    return 0;
  nrf_ll_ce_low();
  if( pipeno )
    *pipeno = ( stat >> 1 ) & 0x07;
  memset( rxdata, 0, NRF_PAYLOAD_SIZE );
  nrf_get_rx_payload( rxdata, NRF_PAYLOAD_SIZE );
  memcpy( pdata, rxdata, maxlen );
  nrf_clear_interrupt( NRF_INT_RX_DR_MASK );
  nrf_ll_ce_high();
  return maxlen;
}

// Sets a random address for the client
void nrf_set_client_address()
{
  u8 i, limit = nrf_p0_addr[ 0 ];

  limit += nrf_p0_addr[ 1 ];
  for( i = 0; i < limit; i ++ )
    nrfh_rand();
  for( i = 2; i < 5; i ++ )
    nrf_p0_addr[ i ] = nrfh_rand() & 0xFF;
  nrf_set_rx_addr( 0, nrf_p0_addr );
}

// ****************************************************************************
// Other public functions

// Initialize nRF interface
void nrf_init()
{
  u8 t[5];
  unsigned i, j;
  
  // Do low-level setup first
  nrf_ll_init();

  nrfprint( "STAT: %d\n", nrf_read_register_byte( NRF_REG_STATUS ) );
  nrfprint( "CONFIG: %d\n", nrf_read_register_byte( NRF_REG_CONFIG ) );
  nrfprint( "ADDRESSES: \n");
  for( i = 0; i < 2; i ++ )
  {
    nrfprint( "  %d -> ", i );
    nrf_get_rx_addr( i, t );
    for( j = 0; j < 5; j ++ )
      nrfprint( "%02X%s", t[ j ], j == 4 ? "\n" : ":" );
  }

  // Setup the actual nRF configuration now
  // Put the chip in low power mode in order to access all its registers
  nrf_write_register_byte( NRF_REG_CONFIG, 0x08 );
  // Enable 'auto acknowledgement' function on all pipes
  nrf_write_register_byte( NRF_REG_EN_AA, 0x3F );
  // 5 bytes for the address field
  nrf_write_register_byte( NRF_REG_SETUP_AW, NRF_SETUP_AW_SIZE_5 );
  // Retry 1 time, start with the default timeout (0)
  // [TODO] uncomment this later
  //nrf_write_register_byte( NRF_REG_SETUP_RETR, 0x01 );
  // Set RF channel
  nrf_write_register_byte( NRF_REG_RF_CH, NRF_CFG_RF_CHANNEL );
  // RF setup (LNA is set to 1)
  nrf_set_rf_setup( NRF_CFG_DATA_RATE, NRF_CFG_TX_POWER, 1 );
  // Setup pipe address(es)
#ifdef NRF_CFG_PROFILE_SERVER
  // Enable the first two pipes
  nrf_write_register_byte( NRF_REG_EN_RXADDR, 0x03 );
  // Set addresses
  nrf_set_rx_addr( 0, nrf_p0_addr );
  nrf_set_rx_addr( 1, nrf_srv_p1_addr );
  // Set payload size
  nrf_set_payload_size( 0, NRF_PAYLOAD_SIZE );
  nrf_set_payload_size( 1, NRF_PAYLOAD_SIZE ); 
#else // #ifdef NRF_CFG_PROFILE_SERVER
  // Enable the first pipe
  nrf_write_register_byte( NRF_REG_EN_RXADDR, 0x01 );
  // Set random address in nrf_p0_addr
  nrf_set_client_address();
  // Set payload size
  nrf_set_payload_size( 0, NRF_PAYLOAD_SIZE );
#endif // #ifdef NRF_CFG_PROFILE_SERVER
  // Finally set the CONFIG register
  // Power up, 2 bytes CRC, interrupts disabled
  // [TODO] change interrupts to enabled eventually
  nrf_write_register_byte( NRF_REG_CONFIG, 0x0E );
#ifdef NRF_CFG_PROFILE_SERVER
  nrf_set_mode( NRF_MODE_RX );
#else
  nrf_set_mode( NRF_MODE_TX );
#endif

  // Power up, transmitter, 2 bytes CRC, interrupts disabled
  nrfprint( "\nAFTER\n\nSTAT: %d\n", nrf_read_register_byte( NRF_REG_STATUS ) );
  nrfprint( "CONFIG: %d\n", nrf_read_register_byte( NRF_REG_CONFIG ) );
  nrfprint( "ADDRESSES: \n");
  for( i = 0; i < 2; i ++ )
  {
    nrfprint( "  %d -> ", i );
    nrf_get_rx_addr( i, t );
    for( j = 0; j < 5; j ++ )
      nrfprint( "%02X%s", t[ j ], j == 4 ? "\n" : ":" );
  }
}

// nRF IRQ handler
void nrf_irq_handler()
{
}

