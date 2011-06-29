/***************************************************************************

    Zilog Z80 Parallel Input/Output Controller implementation

    Copyright Nicola Salmoria and the MAME Team.
    Visit http://mamedev.org for licensing and usage restrictions.

***************************************************************************/

#include "emu.h"
#include "z80pio.h"
#include "cpu/z80/z80daisy.h"


//**************************************************************************
//  CONSTANTS
//**************************************************************************

#define LOG 0

enum
{
	MODE_OUTPUT = 0,
	MODE_INPUT,
	MODE_BIDIRECTIONAL,
	MODE_BIT_CONTROL
};

enum
{
	ANY = 0,
	IOR,
	MASK
};

const int ICW_ENABLE_INT	= 0x80;
const int ICW_AND_OR		= 0x40;
const int ICW_AND			= 0x40;
const int ICW_OR			= 0x00;
const int ICW_HIGH_LOW		= 0x20;
const int ICW_HIGH			= 0x20;
const int ICW_LOW			= 0x00;
const int ICW_MASK_FOLLOWS	= 0x10;



//**************************************************************************
//  LIVE DEVICE
//**************************************************************************

// device type definition
const device_type Z80PIO = &device_creator<z80pio_device>;

//-------------------------------------------------
//  z80pio_device - constructor
//-------------------------------------------------

z80pio_device::z80pio_device(const machine_config &mconfig, const char *tag, device_t *owner, UINT32 clock)
	: device_t(mconfig, Z80PIO, "Z8420", tag, owner, clock),
	  device_z80daisy_interface(mconfig, *this)
{
}


//-------------------------------------------------
//  device_config_complete - perform any
//  operations now that the configuration is
//  complete
//-------------------------------------------------

void z80pio_device::device_config_complete()
{
	// inherit a copy of the static data
	const z80pio_interface *intf = reinterpret_cast<const z80pio_interface *>(static_config());
	if (intf != NULL)
		*static_cast<z80pio_interface *>(this) = *intf;

	// or initialize to defaults if none provided
	else
	{
		memset(&m_out_int_cb, 0, sizeof(m_out_int_cb));
		memset(&m_in_pa_cb, 0, sizeof(m_in_pa_cb));
		memset(&m_out_pa_cb, 0, sizeof(m_out_pa_cb));
		memset(&m_out_ardy_cb, 0, sizeof(m_out_ardy_cb));
		memset(&m_in_pb_cb, 0, sizeof(m_in_pb_cb));
		memset(&m_out_pb_cb, 0, sizeof(m_out_pb_cb));
		memset(&m_out_brdy_cb, 0, sizeof(m_out_brdy_cb));
	}
}


//-------------------------------------------------
//  device_start - device-specific startup
//-------------------------------------------------

void z80pio_device::device_start()
{
	m_port[PORT_A].start(this, PORT_A, m_in_pa_cb, m_out_pa_cb, m_out_ardy_cb);
	m_port[PORT_B].start(this, PORT_B, m_in_pb_cb, m_out_pb_cb, m_out_brdy_cb);

	// resolve callbacks
	m_out_int_func.resolve(m_out_int_cb, *this);
}


//-------------------------------------------------
//  device_reset - device-specific reset
//-------------------------------------------------

void z80pio_device::device_reset()
{
	// loop over ports
	for (int index = PORT_A; index < PORT_COUNT; index++)
		m_port[index].reset();
}



//**************************************************************************
//  DAISY CHAIN INTERFACE
//**************************************************************************

//-------------------------------------------------
//  z80daisy_irq_state - return the overall IRQ
//  state for this device
//-------------------------------------------------

int z80pio_device::z80daisy_irq_state()
{
	int state = 0;

	for (int index = PORT_A; index < PORT_COUNT; index++)
	{
		pio_port &port = m_port[index];

		if (port.m_ius)
		{
			// interrupt under service
			return Z80_DAISY_IEO;
		}
		else if (port.m_ie && port.m_ip)
		{
			// interrupt pending
			state = Z80_DAISY_INT;
		}
	}

	return state;
}


//-------------------------------------------------
//  z80daisy_irq_ack - acknowledge an IRQ and
//  return the appropriate vector
//-------------------------------------------------

int z80pio_device::z80daisy_irq_ack()
{
	for (int index = PORT_A; index < PORT_COUNT; index++)
	{
		pio_port &port = m_port[index];

		if (port.m_ip)
		{
			if (LOG) logerror("Z80PIO '%s' Interrupt Acknowledge\n", tag());

			// clear interrupt pending flag
			port.m_ip = false;

			// set interrupt under service flag
			port.m_ius = true;

			check_interrupts();

			return port.m_vector;
		}
	}

	logerror("z80pio_irq_ack: failed to find an interrupt to ack!\n");

	return 0;
}


//-------------------------------------------------
//  z80daisy_irq_reti - clear the interrupt
//  pending state to allow other interrupts through
//-------------------------------------------------

void z80pio_device::z80daisy_irq_reti()
{
	for (int index = PORT_A; index < PORT_COUNT; index++)
	{
		pio_port &port = m_port[index];

		if (port.m_ius)
		{
			if (LOG) logerror("Z80PIO '%s' Return from Interrupt\n", tag());

			// clear interrupt under service flag
			port.m_ius = false;
			check_interrupts();

			return;
		}
	}

	logerror("z80pio_irq_reti: failed to find an interrupt to clear IEO on!\n");
}



//**************************************************************************
//  DEVICE-LEVEL IMPLEMENTATION
//**************************************************************************

//-------------------------------------------------
//  control_read - control register read
//-------------------------------------------------

UINT8 z80pio_device::control_read()
{
	return (m_port[PORT_A].m_icw & 0xc0) | (m_port[PORT_B].m_icw >> 4);
}


//-------------------------------------------------
//  check_interrupts - update the interrupt state
//  over all ports
//-------------------------------------------------

void z80pio_device::check_interrupts()
{
	int state = CLEAR_LINE;

	for (int index = PORT_A; index < PORT_COUNT; index++)
		if (m_port[index].interrupt_signalled())
			state = ASSERT_LINE;

	m_out_int_func(state);
}



//**************************************************************************
//  PORT-LEVEL IMPLEMENTATION
//**************************************************************************

//-------------------------------------------------
//  pio_port - constructor
//-------------------------------------------------

z80pio_device::pio_port::pio_port()
	: m_device(NULL),
	  m_index(0),
	  m_mode(0),
	  m_next_control_word(0),
	  m_input(0),
	  m_output(0),
	  m_ior(0),
	  m_rdy(false),
	  m_stb(false),
	  m_ie(false),
	  m_ip(false),
	  m_ius(false),
	  m_icw(0),
	  m_vector(0),
	  m_mask(0),
	  m_match(false)
{
	memset(&m_in_p_func, 0, sizeof(m_in_p_func));
	memset(&m_out_p_func, 0, sizeof(m_out_p_func));
	memset(&m_out_rdy_func, 0, sizeof(m_out_rdy_func));
}


//-------------------------------------------------
//  start - set up a port during device startup
//-------------------------------------------------

void z80pio_device::pio_port::start(z80pio_device *device, int index, const devcb_read8 &infunc, const devcb_write8 &outfunc, const devcb_write_line &rdyfunc)
{
	m_device = device;
	m_index = index;

	// resolve callbacks
	m_in_p_func.resolve(infunc, *m_device);
	m_out_p_func.resolve(outfunc, *m_device);
	m_out_rdy_func.resolve(rdyfunc, *m_device);

	// register for state saving
	m_device->save_item(NAME(m_mode), m_index);
	m_device->save_item(NAME(m_next_control_word), m_index);
	m_device->save_item(NAME(m_input), m_index);
	m_device->save_item(NAME(m_output), m_index);
	m_device->save_item(NAME(m_ior), m_index);
	m_device->save_item(NAME(m_rdy), m_index);
	m_device->save_item(NAME(m_stb), m_index);
	m_device->save_item(NAME(m_ie), m_index);
	m_device->save_item(NAME(m_ip), m_index);
	m_device->save_item(NAME(m_ius), m_index);
	m_device->save_item(NAME(m_icw), m_index);
	m_device->save_item(NAME(m_vector), m_index);
	m_device->save_item(NAME(m_mask), m_index);
	m_device->save_item(NAME(m_match), m_index);
}


//-------------------------------------------------
//  reset - reset a port during device reset
//-------------------------------------------------

void z80pio_device::pio_port::reset()
{
	// set mode 1
	set_mode(MODE_INPUT);

	// reset interrupt enable flip-flops
	m_icw &= ~ICW_ENABLE_INT;
	m_ie = false;
	m_ip = false;
	m_ius = false;
	m_match = false;

	// reset all bits of the data I/O register
	m_ior = 0;

	// set all bits of the mask control register
	m_mask = 0xff;

	// reset output register
	m_output = 0;

	// clear ready line
	set_rdy(false);
}


//-------------------------------------------------
//  interrupt_signalled - return true if an
//  interrupt is signalled
//-------------------------------------------------

bool z80pio_device::pio_port::interrupt_signalled()
{
	if (m_mode == MODE_BIT_CONTROL)
	{
		// fetch input data (ignore output lines)
		UINT8 data = (m_input & m_ior) | (m_output & ~m_ior);
		UINT8 mask = ~m_mask;
		bool match = false;

		data &= mask;

		if ((m_icw & 0x60) == 0 && data != mask) match = true;
		else if ((m_icw & 0x60) == 0x20 && data != 0) match = true;
		else if ((m_icw & 0x60) == 0x40 && data == 0) match = true;
		else if ((m_icw & 0x60) == 0x60 && data == mask) match = true;

		if (!m_match && match)
		{
			// trigger interrupt
			m_ip = true;
			if (LOG) logerror("Z80PIO '%s' Port %c Interrupt Pending\n", m_device->tag(), 'A' + m_index);
		}

		m_match = match;
	}

	return (m_ie && m_ip && !m_ius);
}


//-------------------------------------------------
//  trigger_interrupt - trigger an interrupt from
//  this port
//-------------------------------------------------

void z80pio_device::pio_port::trigger_interrupt()
{
	m_ip = true;
	if (LOG) logerror("Z80PIO '%s' Port %c Interrupt Pending\n", m_device->tag(), 'A' + m_index);

	check_interrupts();
}


//-------------------------------------------------
//  set_rdy - set the port's RDY line
//-------------------------------------------------

void z80pio_device::pio_port::set_rdy(bool state)
{
	if (m_rdy == state) return;

	if (LOG) logerror("Z80PIO '%s' Port %c Ready: %u\n", m_device->tag(), 'A' + m_index, state);

	m_rdy = state;
	m_out_rdy_func(state);
}


//-------------------------------------------------
//  set_mode - set the port's mode
//-------------------------------------------------

void z80pio_device::pio_port::set_mode(int mode)
{
	if (LOG) logerror("Z80PIO '%s' Port %c Mode: %u\n", m_device->tag(), 'A' + m_index, mode);

	switch (mode)
	{
	case MODE_OUTPUT:
		// enable data output
		m_out_p_func(0, m_output);

		// assert ready line
		set_rdy(true);

		// set mode register
		m_mode = mode;
		break;

	case MODE_INPUT:
		// set mode register
		m_mode = mode;
		break;

	case MODE_BIDIRECTIONAL:
		if (m_index == PORT_B)
		{
			logerror("Z80PIO '%s' Port %c Invalid Mode: %u!\n", m_device->tag(), 'A' + m_index, mode);
		}
		else
		{
			// set mode register
			m_mode = mode;
		}
		break;

	case MODE_BIT_CONTROL:
		if ((m_index == PORT_A) || (m_device->m_port[PORT_A].m_mode != MODE_BIDIRECTIONAL))
		{
			// clear ready line
			set_rdy(false);
		}

		// disable interrupts until IOR is written
		m_ie = false;
		check_interrupts();

		// set logic equation to false
		m_match = false;

		// next word is I/O register
		m_next_control_word = IOR;

		// set mode register
		m_mode = mode;
		break;
	}
}


//-------------------------------------------------
//  strobe - strobe data in/out of the port
//-------------------------------------------------

void z80pio_device::pio_port::strobe(bool state)
{
	if (LOG) logerror("Z80PIO '%s' Port %c Strobe: %u\n", m_device->tag(), 'A' + m_index, state);

	if (m_device->m_port[PORT_A].m_mode == MODE_BIDIRECTIONAL)
	{
		if (m_rdy) // port ready
		{
			if (m_stb && !state) // falling edge
			{
				if (m_index == PORT_A)
					m_out_p_func(0, m_output);
				else
					m_device->m_port[PORT_A].m_input = m_device->m_port[PORT_A].m_in_p_func(0);
			}
			else if (!m_stb && state) // rising edge
			{
				trigger_interrupt();

				// clear ready line
				set_rdy(false);
			}
		}
	}
	else
	{
		switch (m_mode)
		{
		case MODE_OUTPUT:
			if (m_rdy)
			{
				if (!m_stb && state) // rising edge
				{
					trigger_interrupt();

					// clear ready line
					set_rdy(false);
				}
			}
			break;

		case MODE_INPUT:
			if (!state)
			{
				// input port data
				m_input = m_in_p_func(0);
			}
			else if (!m_stb && state) // rising edge
			{
				trigger_interrupt();

				// clear ready line
				set_rdy(false);
			}
			break;
		}
	}

	m_stb = state;
}


//-------------------------------------------------
//  read - port I/O read
//-------------------------------------------------

UINT8 z80pio_device::pio_port::read()
{
	UINT8 data = 0xff;

	switch (m_mode)
	{
	case MODE_OUTPUT:
		data = m_output;
		break;

	case MODE_BIDIRECTIONAL:
		if (m_index == PORT_A)
			data = m_output;
		break;

	case MODE_BIT_CONTROL:
		data = m_ior | (m_output & (m_ior ^ 0xff));
		break;
	}

	return data;
}


//-------------------------------------------------
//  write - port I/O write
//-------------------------------------------------

void z80pio_device::pio_port::write(UINT8 data)
{
	if (m_mode == MODE_BIT_CONTROL)
	{
		// latch data
		m_input = data;
		check_interrupts();
	}
}


//-------------------------------------------------
//  control_write - control register write
//-------------------------------------------------

void z80pio_device::pio_port::control_write(UINT8 data)
{
	switch (m_next_control_word)
	{
	case ANY:
		if (!BIT(data, 0))
		{
			// load interrupt vector
			m_vector = data;
			if (LOG) logerror("Z80PIO '%s' Port %c Interrupt Vector: %02x\n", m_device->tag(), 'A' + m_index, data);

			// set interrupt enable
			m_icw |= ICW_ENABLE_INT;
			m_ie = true;
			check_interrupts();
		}
		else
		{
			switch (data & 0x0f)
			{
			case 0x0f: // select operating mode
				set_mode(data >> 6);
				break;

			case 0x07: // set interrupt control word
				m_icw = data;

				if (LOG)
				{
					logerror("Z80PIO '%s' Port %c Interrupt Enable: %u\n", m_device->tag(), 'A' + m_index, BIT(data, 7));
					logerror("Z80PIO '%s' Port %c Logic: %s\n", m_device->tag(), 'A' + m_index, BIT(data, 6) ? "AND" : "OR");
					logerror("Z80PIO '%s' Port %c Active %s\n", m_device->tag(), 'A' + m_index, BIT(data, 5) ? "High" : "Low");
					logerror("Z80PIO '%s' Port %c Mask Follows: %u\n", m_device->tag(), 'A' + m_index, BIT(data, 4));
				}

				if (m_icw & ICW_MASK_FOLLOWS)
				{
					// disable interrupts until mask is written
					m_ie = false;

					// reset pending interrupts
					m_ip = false;
					check_interrupts();

					// set logic equation to false
					m_match = false;

					// next word is mask control
					m_next_control_word = MASK;
				}
				break;

			case 0x03: // set interrupt enable flip-flop
				m_icw = (data & 0x80) | (m_icw & 0x7f);
				if (LOG) logerror("Z80PIO '%s' Port %c Interrupt Enable: %u\n", m_device->tag(), 'A' + m_index, BIT(data, 7));

				// set interrupt enable
				m_ie = BIT(m_icw, 7) ? true : false;
				check_interrupts();
				break;

			default:
				logerror("Z80PIO '%s' Port %c Invalid Control Word: %02x!\n", m_device->tag(), 'A' + m_index, data);
			}
		}
		break;

	case IOR: // data direction register
		m_ior = data;
		if (LOG) logerror("Z80PIO '%s' Port %c IOR: %02x\n", m_device->tag(), 'A' + m_index, data);

		// set interrupt enable
		m_ie = BIT(m_icw, 7) ? true : false;
		check_interrupts();

		// next word is any
		m_next_control_word = ANY;
		break;

	case MASK: // interrupt mask
		m_mask = data;
		if (LOG) logerror("Z80PIO '%s' Port %c Mask: %02x\n", m_device->tag(), 'A' + m_index, data);

		// set interrupt enable
		m_ie = BIT(m_icw, 7) ? true : false;
		check_interrupts();

		// next word is any
		m_next_control_word = ANY;
		break;
	}
}


//-------------------------------------------------
//  data_read - data register read
//-------------------------------------------------

UINT8 z80pio_device::pio_port::data_read()
{
	UINT8 data = 0;

	switch (m_mode)
	{
	case MODE_OUTPUT:
		data = m_output;
		break;

	case MODE_INPUT:
		if (!m_stb)
		{
			// input port data
			m_input = m_in_p_func(0);
		}

		data = m_input;

		// clear ready line
		set_rdy(false);

		// assert ready line
		set_rdy(true);
		break;

	case MODE_BIDIRECTIONAL:
		data = m_input;

		// clear ready line
		m_device->m_port[PORT_B].set_rdy(false);

		// assert ready line
		m_device->m_port[PORT_B].set_rdy(true);
		break;

	case MODE_BIT_CONTROL:
		// input port data
		m_input = m_in_p_func(0);

		data = (m_input & m_ior) | (m_output & (m_ior ^ 0xff));
		break;
	}

	return data;
}


//-------------------------------------------------
//  data_write - data register write
//-------------------------------------------------

void z80pio_device::pio_port::data_write(UINT8 data)
{
	switch (m_mode)
	{
	case MODE_OUTPUT:
		// clear ready line
		set_rdy(false);

		// latch output data
		m_output = data;

		// output data to port
		m_out_p_func(0, data);

		// assert ready line
		set_rdy(true);
		break;

	case MODE_INPUT:
		// latch output data
		m_output = data;
		break;

	case MODE_BIDIRECTIONAL:
		// clear ready line
		set_rdy(false);

		// latch output data
		m_output = data;

		if (!m_stb)
		{
			// output data to port
			m_out_p_func(0, data);
		}

		// assert ready line
		set_rdy(true);
		break;

	case MODE_BIT_CONTROL:
		// latch output data
		m_output = data;

		// output data to port
		m_out_p_func(0, m_ior | (m_output & (m_ior ^ 0xff)));
		break;
	}
}



//**************************************************************************
//  GLOBAL STUBS
//**************************************************************************

READ8_DEVICE_HANDLER( z80pio_c_r ) { return downcast<z80pio_device *>(device)->control_read(); }
WRITE8_DEVICE_HANDLER( z80pio_c_w ) { downcast<z80pio_device *>(device)->control_write(offset & 1, data); }

READ8_DEVICE_HANDLER( z80pio_d_r ) { return downcast<z80pio_device *>(device)->data_read(offset & 1); }
WRITE8_DEVICE_HANDLER( z80pio_d_w ) { downcast<z80pio_device *>(device)->data_write(offset & 1, data); }

READ_LINE_DEVICE_HANDLER( z80pio_ardy_r ) { return downcast<z80pio_device *>(device)->rdy(z80pio_device::PORT_A); }
READ_LINE_DEVICE_HANDLER( z80pio_brdy_r ) { return downcast<z80pio_device *>(device)->rdy(z80pio_device::PORT_B); }

WRITE_LINE_DEVICE_HANDLER( z80pio_astb_w ) { downcast<z80pio_device *>(device)->strobe(z80pio_device::PORT_A, state ? true : false); }
WRITE_LINE_DEVICE_HANDLER( z80pio_bstb_w ) { downcast<z80pio_device *>(device)->strobe(z80pio_device::PORT_B, state ? true : false); }

READ8_DEVICE_HANDLER( z80pio_pa_r ) { return downcast<z80pio_device *>(device)->port_read(z80pio_device::PORT_A); }
READ8_DEVICE_HANDLER( z80pio_pb_r ) { return downcast<z80pio_device *>(device)->port_read(z80pio_device::PORT_B); }

WRITE8_DEVICE_HANDLER( z80pio_pa_w ) { downcast<z80pio_device *>(device)->port_write(z80pio_device::PORT_A, data); }
WRITE8_DEVICE_HANDLER( z80pio_pb_w ) { downcast<z80pio_device *>(device)->port_write(z80pio_device::PORT_B, data); }

//-------------------------------------------------
//  z80pio_cd_ba_r - register read
//-------------------------------------------------

READ8_DEVICE_HANDLER( z80pio_cd_ba_r )
{
	int index = BIT(offset, 0);

	return BIT(offset, 1) ? z80pio_c_r(device, index) : z80pio_d_r(device, index);
}

//-------------------------------------------------
//  z80pio_cd_ba_w - register write
//-------------------------------------------------

WRITE8_DEVICE_HANDLER( z80pio_cd_ba_w )
{
	int index = BIT(offset, 0);

	BIT(offset, 1) ? z80pio_c_w(device, index, data) : z80pio_d_w(device, index, data);
}

//-------------------------------------------------
//  z80pio_ba_cd_r - register read
//-------------------------------------------------

READ8_DEVICE_HANDLER( z80pio_ba_cd_r )
{
	int index = BIT(offset, 1);

	return BIT(offset, 0) ? z80pio_c_r(device, index) : z80pio_d_r(device, index);
}

//-------------------------------------------------
//  z80pio_ba_cd_w - register write
//-------------------------------------------------

WRITE8_DEVICE_HANDLER( z80pio_ba_cd_w )
{
	int index = BIT(offset, 1);

	BIT(offset, 0) ? z80pio_c_w(device, index, data) : z80pio_d_w(device, index, data);
}
