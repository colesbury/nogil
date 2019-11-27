/*
 * Stubs for when the GIL is disabled
 */

static void
_gil_initialize(struct _gil_runtime_state *gil)
{
}

static int _gil_created = 0;

static int
gil_created(struct _gil_runtime_state *gil)
{
    return _gil_created;
}

static void
create_gil(struct _gil_runtime_state *gil)
{
    _gil_created = 1;
}

static void
destroy_gil(struct _gil_runtime_state *gil)
{
    _gil_created = 0;
}

static void
recreate_gil(struct _gil_runtime_state *gil)
{
    _gil_created = 1;
}

static void
drop_gil(struct _ceval_runtime_state *ceval, PyThreadState *tstate)
{
}

static void
take_gil(struct _ceval_runtime_state *ceval, PyThreadState *tstate)
{
}

static unsigned long _gil_switch_interval = 5000;

void
_PyEval_SetSwitchInterval(unsigned long microseconds)
{
	_gil_switch_interval = microseconds;
}

unsigned long
_PyEval_GetSwitchInterval()
{
    return _gil_switch_interval;
}