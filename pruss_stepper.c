
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <dirent.h>
#include <string.h> 
#include <sched.h>
#include <unistd.h>

#include "pruss_stepper.h"
#define PRU_NR		1
#include "pruss.h"
#include "beaglebone.h"
#include "debug.h"
#include "bebopr.h"

// Generic struct for access to 'command' field for all commands.
typedef struct {
  unsigned int			: 24;
  unsigned int	value		:  4;
  unsigned int	axis		:  3;
  unsigned int                  :  1;
} CommandStruct;

// CMD_AXIS_SET_ORIGIN
typedef struct {
  unsigned int			: 24;
  unsigned int	command		:  4;
  unsigned int	axis		:  3;
  unsigned int			:  1;
  unsigned int	position	: 32;
} SetOriginStruct;

// CMD_AXIS_SET_ACCEL
typedef struct {
  unsigned int	accelCount	: 24;
  unsigned int	command		:  4;
  unsigned int	axis		:  3;
  unsigned int			:  1;
  unsigned int			: 32;
  unsigned int	stepCycle	: 32;
} SetAccelStruct;

// CMD_AXIS_RAMP_UP
typedef struct {
  unsigned int			: 24;
  unsigned int	command		:  4;
  unsigned int	axis		:  3;
  unsigned int			:  1;
  int		moveDelta	: 32;
  unsigned int	stepCycle	: 32;
} AccelStruct;

// CMD_AXIS_RAMP_DOWN
typedef struct {
  unsigned int			: 24;
  unsigned int	command		:  4;
  unsigned int	axis		:  3;
  unsigned int			:  1;
  int		moveDelta	: 32;
  unsigned int	stepCycle	: 32;
} DecelStruct;

// CMD_AXIS_DWELL
typedef struct {
  unsigned int			: 24;
  unsigned int	command		:  4;
  unsigned int	axis		:  3;
  unsigned int			:  1;
  int		moveDelta	: 32;
  unsigned int	stepCycle	: 32;
} DwellStruct;

// CMD_AXIS_SET_PULSE_LENGTH
typedef struct {
  unsigned int	duration	: 24;
  unsigned int	command		:  4;
  unsigned int	axis		:  3;
} SetPulseStruct;

// CMD_SET_IDLE_TIMEOUT
typedef struct {
  unsigned int	timeout		:  8;
  unsigned int			: 16;
  unsigned int	command		:  4;
} SetIdleTimeoutStruct;

// CMD_SET_ENABLE
typedef struct {
  unsigned int	mode		:  8;
  unsigned int			: 16;
  unsigned int	command		:  4;
} SetEnableStruct;

// CMD_AXIS_CONFIG_AXIS
typedef struct {
  unsigned int	reverse		:  1;
  unsigned int			: 23;
  unsigned int	command		:  4;
  unsigned int	axis		:  3;
  unsigned int			:  1;
  unsigned int	stepSizeT	: 16;
  unsigned int	stepSizeN	: 16;
  unsigned int	stepSize	: 32;
} ConfigAxisStruct;

// CMD_AXIS_ADJUST_ORIGIN
typedef struct {
  unsigned int			: 24;
  unsigned int	command		:  4;
  unsigned int	axis		:  3;
  unsigned int			:  1;
  unsigned int	position	: 32;
} AdjustOriginStruct;

// CMD_AXIS_ADJUST_FOR_RAMP
typedef struct {
  unsigned int			: 24;
  unsigned int	command		:  4;
  unsigned int	axis		:  3;
  unsigned int			:  1;
  int		delta		: 32;
} AdjustForRampStruct;

/* TODO: can we mmap an array of this struct directly onto SRAM ? */
typedef union {
  uint32_t		gen[ 4];
  CommandStruct		command;
  SetOriginStruct	set_origin;
  SetAccelStruct	set_accel;
  AccelStruct		accel;
  DecelStruct		decel;
  DwellStruct		dwell;
  SetPulseStruct	set_pulse;
  SetIdleTimeoutStruct	timeout;
  SetEnableStruct	enable;
  ConfigAxisStruct	config;
  AdjustOriginStruct	adjust_origin;
  AdjustForRampStruct	adjust_for_ramp;
} PruCommandUnion;


#define IX_IN		(PRUSS_RAM_OFFSET + 0xC0)
#define IX_OUT		(PRUSS_RAM_OFFSET + 0xC1)
#define BUSY_FLAG	(PRUSS_RAM_OFFSET + 0xC2)

static int pruss_ecap_init( void)
{
#define O_TSCTR		0
#define O_CTRPHS	4
#define O_CAP1		8	/* cycle */
#define O_CAP2		12	/* compare / duty cycle */
#define O_CAP3		16
#define O_CAP4		20
#define O_ECCTL1	40
#define O_ECCTL2	42
#define O_ECEINT	44
#define O_ECFLG		46
#define O_ECCLR		48
#define O_ECFRC		50
#define O_REVID		92

  uint32_t eCapId = pruss_rd32( PRUSS_ECAP0_OFFSET + O_REVID);
  uint32_t value;

  if (eCapId != 0x44d22100) {
    printf( "*** ERROR: invalid eCapId! Found 0x%08x, should be 0x44d22100\n", eCapId);
    exit( EXIT_FAILURE);
  }
  // Stop counter
  pruss_wr16( PRUSS_ECAP0_OFFSET + O_ECCTL2, (1 << 9) | (1 << 7) | (0 << 4));
  // Reset counter and phase registers
  pruss_wr32( PRUSS_ECAP0_OFFSET + O_TSCTR, 0);
  pruss_wr32( PRUSS_ECAP0_OFFSET + O_CTRPHS, 0);
  // Setup for large (detection) cycle
  pruss_wr32( PRUSS_ECAP0_OFFSET + O_CAP1, 20000000);	// PWM period 0.1 s
  pruss_wr32( PRUSS_ECAP0_OFFSET + O_CAP2, 10000000);	// PWM on time, don't care
  pruss_wr32( PRUSS_ECAP0_OFFSET + O_CAP1, 200);		// set PWM period to 1 us 
  pruss_wr32( PRUSS_ECAP0_OFFSET + O_CAP2, 100);		// PWM on time, don't care
  // Clear pending interrupt
  pruss_wr16( PRUSS_ECAP0_OFFSET + O_ECCLR, (1 << 6));
  if (pruss_rd16( PRUSS_ECAP0_OFFSET + O_ECFLG) & (1 << 6)) {
    printf( "*** WARNING: could not clear eCAP0 interrupt\n");
    exit( EXIT_FAILURE);
  }
  // Start counter in APWM mode
  pruss_wr16( PRUSS_ECAP0_OFFSET + O_ECCTL2, (1 << 9) | (1 << 7) | (1 << 4));
  do {
    value = pruss_rd16( PRUSS_ECAP0_OFFSET + O_ECFLG);
  } while ((value & (1 << 6)) == 0);
  pruss_wr16( PRUSS_ECAP0_OFFSET + O_ECCLR, (1 << 6));
  if (debug_flags & DEBUG_PRUSS) {
    printf( "eCap0 initialized and found operational\n");
  }
  return 0;
}

#define UCODENAME "stepper.bin"

static int pruss_command( PruCommandUnion* cmd);

int pruss_stepper_init( void)
{
  struct ucode_signature signature;

  if (pruss_init( UCODENAME, &signature) < 0) {
    return -1;
  }
  if (signature.ucode_magic == UCODE_MAGIC && signature.fw_version == FW_VERSION) {
    if (debug_flags & DEBUG_PRUSS) {
      printf( "Valid STEPPER microcode found (version %d.%d).\n",
	      signature.fw_version, signature.fw_revision);
    }
  } else {
    if (signature.ucode_magic == UCODE_MAGIC) {
      // This is stepper code, must be an incompatible version
      fprintf( stderr, "ERROR: the code in file '%s' (version %d.%d) is not compatible with this version %d.x!\n",
	      UCODENAME, signature.fw_version, signature.fw_revision, FW_VERSION);
    } else {
      // This is not stepper code.
      fprintf( stderr, "ERROR: the code in file '%s' is not STEPPER firmware!\n", UCODENAME);
    }
    return -1;
  }
  if (pruss_ecap_init() < 0) {
    return -1;
  }

  int ix_out = 0;
  int ix_in  = 0;
  if (debug_flags & DEBUG_PRUSS) {
    printf( "Setting FIFO pointers to %d (in) and %d (out).\n", ix_in, ix_out);
  }
  pruss_wr8( IX_IN, ix_in);		// in
  pruss_wr8( IX_OUT, ix_out);		// out
//  pruss_wr16( IX_OUT + 1, 0xdeaf);	// filler

 /*
  * Now start the code: Enable the PRUSS.
  */
  uint16_t pc = pruss_rd16( PRUSS_PRU_CTRL_STATUS);
  pruss_start_pruss();

  if (debug_flags & DEBUG_PRUSS) {
    printf( "PRUSS successfully started at PC=%d.\n", pc);
  }
  for (int i = 1 ; i <= 4 ; ++i) {
    if (pruss_queue_set_origin( i) < 0) {
      fprintf( stderr, "Failed to execute PRUSS queue command.\n");
      exit( EXIT_FAILURE);
    }
  }
  PruCommandUnion pruCmd;
  pruCmd.enable.command	= CMD_SET_ENABLE;
  pruCmd.enable.mode	= (config_use_pololu_drivers()) ? 2 : 3;
  if (pruss_command( &pruCmd) < 0) {
    return -1;
  }
  if (DEBUG_PRUSS) {
    debug_flags &= ~DEBUG_PRUSS;
  }
  return 0;
}

int pruss_stepper_dump_state( void)
{
  int i;

  int pruss_ena = pruss_dump_state();

  /*
   *  Dump contents of shared memory.
   *  The pruss needn't be halted to do this, but if not, memory could change!
   */
  unsigned int ret = pruss_rd16( PRUSS_DBG_OFFSET + 4 * 2);		// R2.w0
  unsigned int sp  = pruss_rd16( PRUSS_DBG_OFFSET + 4 * 2 + 2);	// R2.w2
  unsigned int pc  = pruss_rd16( PRUSS_CTL_OFFSET + 4);
  printf( "PC == 0x%03x (%d), SP == 0x%03x (%d), RET == 0x%04x (%4d)\n", pc, pc, sp, sp, ret, ret);
  for ( sp = (sp & 0xFFF0), i = 0 ; i < 16 ; ++i) {
    ret = pruss_rd16( PRUSS_RAM_OFFSET + sp);
    if (ret == 0xdead || ret == 0xbeef) {
      break;
    }
    printf( "Stack[%3d] @ SRAM[ 0x%03x] == 0x%04x (%5d)\n", i, sp, ret, ret);
    sp += 2;
  }

#define	FullADSize		(12 * 4)

  /* Dump per axis data */
  {
    uint32_t base[ 4];
    uint32_t data[ 4];
    uint16_t axes_config = pruss_rd16( PRUSS_DBG_OFFSET + 4 * 11 + 2);

#define DUMP_LINE( field, size, offset, format)     			          \
    do {                                                                          \
      int i;                                                                      \
      for (i = 0 ; i < 4 ; ++i) {				                  \
        data[ i] = pruss_rd ## size( PRUSS_RAM_OFFSET + i * FullADSize + offset); \
      }									          \
      printf( "%20s    " format "    " format "    " format "    " format "\n",   \
	       field, data[ 0], data[ 1], data[ 2], data[ 3]);			  \
    } while (0)

    printf( "%20s    %14s    %14s    %14s    %14s\n", "variable", "X-axis", "Y-axis", "Z-axis", "E-axis");
    for (i = 0 ; i < 4 ; ++i) {
      base[ i] = PRUSS_RAM_OFFSET + i * FullADSize;
    }
    printf( "%20s    %14x    %14x    %14x    %14x\n", "Base Address x", base[ 0], base[ 1], base[ 2], base[ 3]);

    DUMP_LINE( "cycleTimer .",       32,  0, "%14u");
    DUMP_LINE( "stepActiveTime .",   32,  4, "%14u");
    DUMP_LINE( "minStepCycleTime .", 32,  8, "%14u");
    DUMP_LINE( "stepCycleTime .",    32, 12, "%14u");
    DUMP_LINE( "stateInfo x",         8, 16, "%14x");
    DUMP_LINE( "stepPinBitNo .",      8, 17, "%14u");
    // following code uses stepPinBitNo from data[] !
    for (i = 0 ; i < 4 ; ++i) {
      data[ i] = !!(axes_config & (1 << data[ i]));
    }
    printf( "%20s    %14x    %14x    %14x    %14x\n", "Axis reversal x", data[ 0], data[ 1], data[ 2], data[ 3]);

    DUMP_LINE( "virtPosT .",         16, 18, "%14u");
    DUMP_LINE( "virtPos x",          32, 20, "%14x");

    DUMP_LINE( "stepSize .",         32, 24, "%14u");
    DUMP_LINE( "stepSizeT .",        16, 28, "%14u");
    DUMP_LINE( "stepSizeN .",        16, 30, "%14u");

    DUMP_LINE( "requestedPos x",     32, 32, "%14x");
    printf( "%20s    %14d    %14d    %14d    %14d\n", "requestedPos .",
	    data[ 0] - VIRT_POS_MID_SCALE,
	    data[ 1] - VIRT_POS_MID_SCALE,
	    data[ 2] - VIRT_POS_MID_SCALE,
	    data[ 3] - VIRT_POS_MID_SCALE);
    DUMP_LINE( "nextStepCycleTime .",32, 36, "%14u");
    DUMP_LINE( "accelCount .",       32, 40, "%14u");
    DUMP_LINE( "dividend .",         32, 44, "%14u");
  }
  if (pruss_ena) {
    // Set bit15 in R6 to signal the PRUSS we're resuming from suspend
    uint32_t reg = pruss_rd32( PRUSS_DBG_OFFSET + 6 * 4);
    pruss_wr32( PRUSS_DBG_OFFSET + 6 * 4, reg | (1 << 15));
    pruss_start_pruss();
    printf( "PRUSS is resuming execution (enabled)\n");
  }
  return 0;
}

static inline int pruss_get_nr_of_free_buffers( void)
{
  int ix_in  = pruss_rd8( IX_IN);
  int ix_out = pruss_rd8( IX_OUT);
  // original formula: NR_CMD_FIFO_ENTRIES - 1 - (NR_CMD_FIFO_ENTRIES + ix_in - ix_out) % NR_CMD_FIFO_ENTRIES;
  return ((ix_out > ix_in) ? 0 : NR_CMD_FIFO_ENTRIES) + ix_out - ix_in - 1;
}

int pruss_queue_full( void)
{
  return (pruss_get_nr_of_free_buffers() == 0);
}

int pruss_queue_empty( void)
{
  // Note that one buffer cannot be used because of the two indexes scheme!
  return (pruss_get_nr_of_free_buffers() == NR_CMD_FIFO_ENTRIES - 1);
}

// Simple wrapper prevents need for pruss.h inclusion
int pruss_stepper_halted( void)
{
  return pruss_is_halted();
}

int pruss_wait_for_queue_space( void)
{
  while (pruss_queue_full()) {
    if (pruss_is_halted()) {
      return -1;
    }
    /*
     * Most time is spent here polling for PRUSS progress.
     * Until an interrupt driven interface is implemented, reduce the
     * cpu load and number of poll cycles by sleeping part of the time.
     */
    usleep( 50);
  }
  return 0;
}

int pruss_stepper_busy( void)
{
  return pruss_rd8( BUSY_FLAG);
}

// Write command structure to buffer[ ix_in] on PRUSS (must be free)
int pruss_write_command_struct( int ix_in, PruCommandUnion* data)
{
  uint32_t a = ix_in * sizeof( data->gen);
  for (int i = 0 ; i < NR_ITEMS( data->gen) ; ++i) {
    uint32_t u = data->gen[ i];
    pruss_wr32( PRUSS_RAM_OFFSET + 1 * 256 + a, u);
//    printf( "pruss_write_command_struct: wrote 0x%08x (%d) to offset %d\n", u, u, a);
    a += sizeof( *data->gen);
  }
  if (++ix_in >= NR_CMD_FIFO_ENTRIES) {
    ix_in = 0;
  }
  pruss_wr8( IX_IN, ix_in);
  return ix_in;
}

// Write command structure to PRUSS, wait for free buffer is nescessary
static int pruss_command( PruCommandUnion* cmd)
{
  int ix_in = pruss_rd8( IX_IN);
  //  int ix_out = pruss_rd8( IX_OUT);
  if (pruss_wait_for_queue_space() < 0) {
    pruss_stepper_dump_state();
    printf( "ERROR: found pruss halted waiting for queue space for command %d, bailing out!\n",
	    cmd->command.value);
    exit( EXIT_FAILURE);
  }
  //  printf( "pruss_command - write to SRAM buffer at index %d, out index is %d.\n", ix_in, ix_out);
  (void) pruss_write_command_struct( ix_in, cmd);
  //  ix_in = writeCommandStruct( ix_in, cmd);
  //  ix_out = pruss_rd8( PRUSS_RAM_OFFSET + 129);
  return 0;
}

/*
 *  PRUSS STEPPER.BIN COMMAND INTERFACE
 */

int pruss_queue_set_origin( int axis)
{
  PruCommandUnion pruCmd = {
    .set_origin.command		= CMD_AXIS_SET_ORIGIN,
    .set_origin.axis		= axis,
    .set_origin.position 	= VIRT_POS_MID_SCALE
  };
  if (pruss_command( &pruCmd) < 0) {
    return -1;
  }
  return 0;
}

int pruss_queue_adjust_origin( int axis)
{
  PruCommandUnion pruCmd = {
    .adjust_origin.command	= CMD_AXIS_ADJUST_ORIGIN,
    .adjust_origin.axis		= axis,
    .adjust_origin.position 	= VIRT_POS_MID_SCALE
  };
  if (pruss_command( &pruCmd) < 0) {
    return -1;
  }
  return 0;
}

int pruss_queue_adjust_for_ramp( int axis, int32_t delta)
{
  PruCommandUnion pruCmd = {
    .adjust_for_ramp.command	= CMD_AXIS_ADJUST_FOR_RAMP,
    .adjust_for_ramp.axis	= axis,
    .adjust_for_ramp.delta	= delta
  };
  if (pruss_command( &pruCmd) < 0) {
    return -1;
  }
  return 0;
}

int pruss_queue_set_pulse_length( int axis, uint16_t length)
{
  PruCommandUnion pruCmd = {
    .set_pulse.command		= CMD_AXIS_SET_PULSE_LENGTH,
    .set_pulse.axis		= axis,
    .set_pulse.duration 	= length
  };
  if (pruss_command( &pruCmd) < 0) {
    return -1;
  }
  return 0;
}

int pruss_queue_set_accel( int axis, uint32_t c0)
{
  PruCommandUnion pruCmd = {
    .set_accel.command		= CMD_AXIS_SET_ACCEL,
    .set_accel.axis		= axis,
    .set_accel.accelCount 	= 0,
    .set_accel.stepCycle 	= c0
  };
  if (pruss_command( &pruCmd) < 0) {
    return -1;
  }
  return 0;
}

int pruss_queue_accel_more( int axis, uint32_t cmin, int32_t delta)
{
  PruCommandUnion pruCmd = {
    .accel.command		= CMD_AXIS_RAMP_UP,
    .accel.axis			= axis,
    .accel.moveDelta		= delta,
    .accel.stepCycle		= cmin
  };
  if (pruss_command( &pruCmd) < 0) {
    return -1;
  }
  return 0;
}

int pruss_queue_accel( int axis, uint32_t c0, uint32_t cmin, int32_t delta)
{
  if (pruss_queue_set_accel( axis, c0) < 0) {
    return -1;
  }
  return pruss_queue_accel_more( axis, cmin, delta);
}

int pruss_queue_dwell( int axis, uint32_t cmin, int32_t delta)
{
  PruCommandUnion pruCmd = {
    .dwell.command		= CMD_AXIS_DWELL,
    .dwell.axis			= axis,
    .dwell.moveDelta		= delta,
    .dwell.stepCycle		= cmin
  };
  if (pruss_command( &pruCmd) < 0) {
    return -1;
  }
  return 0;
}

int pruss_queue_decel( int axis, int32_t delta)
{
  PruCommandUnion pruCmd = {
    .decel.command		= CMD_AXIS_RAMP_DOWN,
    .decel.axis			= axis,
    .decel.moveDelta		= delta
  };
  if (pruss_command( &pruCmd) < 0) {
    return -1;
  }
  return 0;
}

int pruss_queue_execute( void)
{
  if (pruss_is_halted()) {
    fprintf( stderr, "FATAL: PRUSS found halted when queueing execute command\n");
    pruss_stepper_dump_state();
    exit( EXIT_FAILURE);
  }
  //  fprintf( stderr, "pruss_queue_execute(): free buffers = %d\n",
  //	   pruss_get_nr_of_free_buffers());
  PruCommandUnion pruCmd = {
    .command.value		= CMD_AXES_EXECUTE,
  };
  if (pruss_command( &pruCmd) < 0) {
    return -1;
  }
  return 0;
}

int pruss_queue_config_axis( int axis, uint32_t ssi, uint16_t sst, uint16_t ssn, int reverse)
{
  PruCommandUnion pruCmd = {
    .config.command		= CMD_AXIS_CONFIG_AXIS,
    .config.axis		= axis,
    .config.reverse		= (reverse) ? 1 : 0,
    .config.stepSize		= ssi,
    .config.stepSizeT		= sst,
    .config.stepSizeN		= ssn,
  };
  if (pruss_command( &pruCmd) < 0) {
    return -1;
  }
  return 0;
}

/*
 * If the idle timeout is not set to 0, the motors will be enabled
 * and disabled automatically. The first step pulse asserts the
 * enable outputs, (period/10) seconds after the last tep pulse,
 * the enables are negated again.
 */
int pruss_queue_set_idle_timeout( uint8_t period)
{
  PruCommandUnion pruCmd = {
    .timeout.command		= CMD_SET_IDLE_TIMEOUT,
    .timeout.timeout		= period,
  };
  if (pruss_command( &pruCmd) < 0) {
    return -1;
  }
  return 0;
}

/*
 * Manually enable or disable the stepperdriver enable signals.
 * Necessary if the automatic idle timeout is disabled.
 */
int pruss_queue_set_enable( int on)
{
  PruCommandUnion pruCmd = {
    .enable.command		= CMD_SET_ENABLE,
    .enable.mode		= (on) ? 1 : 0,
  };
  if (pruss_command( &pruCmd) < 0) {
    return -1;
  }
  return 0;
}

int pruss_dump_position( int axis)
{
  const char axes[] = { '?', 'X', 'Y', 'Z', 'E' };

  //  if (axis >= 1 && axis <= 4) {
  printf( "\n");
  for (axis = 1 ; axis <= 4 ; ++axis) {
    uint32_t base = PRUSS_RAM_OFFSET + (axis - 1) * FullADSize;
    int32_t virtPosI = pruss_rd32( base + 20) - VIRT_POS_MID_SCALE;
    uint16_t virtPosT = pruss_rd16( base + 18);
    uint16_t stepSizeN = pruss_rd16( base + 30);
    uint16_t stepSizeT = pruss_rd16( base + 28);
    if (stepSizeT == 0) {
      printf( "  %c-virtPos = %d", axes[ axis], virtPosI);
    } else {
      printf( "  %c-virtPos = %d+%u/%u", axes[ axis], virtPosI, virtPosT, stepSizeN);
    }
  }
  printf( "\n");
  return 0;
}

/*
 *
 */
