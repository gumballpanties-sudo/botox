#pragma once
#include <utility>

namespace n_render_queue
{
	/* CFunctor layout: IRefCounted add_ref / release, virtual dtor, operator(). vtable order matters:
	   the queue calls slot 3 to run and slot 2 to destroy; a wrong slot jumps into garbage. */
	class c_functor
	{
	public:
		virtual int add_ref( )    = 0;
		virtual int release( )    = 0;
		virtual ~c_functor( )     = default;
		virtual void operator( )( ) = 0;

		// CFunctor::m_nUserID, only debug builds write it, but the layout needs it
		unsigned int m_user_id = 0;
	};

	int pending( );

	// false once eject starts: queued calls played after release_render_resources would rebuild shaders + run past the module free
	bool live( );

	bool submit_functor( c_functor* functor );

	void stop( );

	template< typename fn_t >
	class c_call final : public c_functor
	{
	public:
		explicit c_call( fn_t&& fn );

		~c_call( ) override;

		int add_ref( ) override { return ++this->m_references; }

		int release( ) override
		{
			const int left = --this->m_references;

			if ( left <= 0 ) {
				delete this;
				return 0;
			}

			return left;
		}

		void operator( )( ) override
		{
			if ( live( ) )
				this->m_fn( );
		}

	private:
		fn_t m_fn;

		int m_references = 0;
	};

	void count_created( );
	void count_destroyed( );

	template< typename fn_t >
	c_call< fn_t >::c_call( fn_t&& fn ) : m_fn( std::move( fn ) )
	{
		count_created( );
	}

	template< typename fn_t >
	c_call< fn_t >::~c_call( )
	{
		count_destroyed( );
	}

	// true = queued for the render thread. false = do it yourself now
	template< typename fn_t >
	bool submit( fn_t fn )
	{
		// unloading: "queued" = caller skips its inline fallback, the call is dropped
		if ( !live( ) )
			return true;

		auto* call = new c_call< fn_t >( std::move( fn ) );

		if ( !submit_functor( call ) ) {
			// never handed over, no queue reference was added
			delete call;
			return false;
		}

		return true;
	}
}
