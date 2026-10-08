#include "render_queue.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"

#include <atomic>

// submitted, not destroyed. written from both threads, read by unload
static std::atomic< int > g_pending{ 0 };

// set once on unload, never cleared
static std::atomic< bool > g_stopping{ false };

void n_render_queue::stop( )
{
	g_stopping.store( true );
}

void n_render_queue::count_created( )
{
	++g_pending;
}

void n_render_queue::count_destroyed( )
{
	--g_pending;
}

int n_render_queue::pending( )
{
	return g_pending.load( );
}

bool n_render_queue::live( )
{
	return !g_stopping.load( ) && !g_ctx.m_unloading.load( std::memory_order_relaxed );
}

bool n_render_queue::submit_functor( c_functor* functor )
{
	if ( !functor || g_stopping.load( ) )
		return false;

	c_material_system* materials = g_interfaces.m_material_system;

	if ( !materials )
		return false;

	c_material_render_context* context = materials->get_render_context( );

	if ( !context )
		return false;

	// null = hardware context, d3d on this thread: caller does the work
	c_call_queue* queue = context->get_call_queue( );

	if ( !queue )
		return false;

	static bool logged = false;

	if ( !logged ) {
		logged = true;

		g_console.print( "render queue: screen passes are going through the material system's render thread — mat_queue_mode left alone" );
	}

	functor->add_ref( );

	queue->queue_functor_internal( functor );

	return true;
}
