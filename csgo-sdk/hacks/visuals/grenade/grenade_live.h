#pragma once
#include "../../../game/sdk/classes/c_vector.h"

#include <vector>

class c_base_entity;
class c_material;
class c_view_setup;

namespace n_grenade_live
{
	enum e_kind : int {
		kind_ignored = -1,
		kind_he      = 0,
		kind_molotov,
		kind_inferno
	};

	struct record_t {
		c_base_entity* m_entity = nullptr;
		int m_index             = 0;
		int m_class             = -1;
		int m_kind              = kind_ignored;
		float m_creation        = 0.f;
		float m_end             = 0.f;

		c_vector m_sim_origin{ }, m_sim_velocity{ };
		std::vector< c_vector > m_path{ };
		c_vector m_point{ };
		bool m_has_point  = false;
		bool m_suppressed = false;
		bool m_threat     = false;

		int m_damage_tick = -1;
		float m_damage    = 0.f;
	};

	struct impl_t {
		void on_paint_traverse( );
		void on_post_screen_space_effects( const c_view_setup* setup );
		void reset( );
		void release_material( );

	private:
		void update_records( );
		bool admit( record_t& record, c_base_entity* entity, int class_id );
		void simulate( record_t& record );
		float damage( record_t& record );
		void draw_warning( record_t& record );

		std::vector< record_t > m_records{ };
		c_material* m_material = nullptr;
	};
}

inline n_grenade_live::impl_t g_grenade_live{ };
