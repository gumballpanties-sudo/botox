#pragma once

/* level is a template arg so call sites stay unchanged; the work is out of line in emit (800 call sites) */
template< n_console::log_level lev >
void n_console::impl_t::print( const char* text )
{
	this->emit( lev, text );
}
