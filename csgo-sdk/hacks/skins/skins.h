#pragma once

#include <string>
#include <vector>

namespace n_skins
{
	struct paint_kit_entry_t {
		int m_id;
		std::string m_name;
	};

	struct paint_colors_t {
		float m_rgb[ 4 ][ 3 ];
	};

	bool custom_colors_for_material( const char* vmt_path, int paint_kit, paint_colors_t& out );

	const char* knife_anim_redirect( const char* requested_model );

	bool knife_anims_fit( int anim_index, int knife_index );

	const char* knife_killfeed_name( );

	void cache_actual_colors( int paint_kit, const paint_colors_t& colors );

	// false = kit never seen composited
	bool get_actual_colors( int paint_kit, paint_colors_t& out );

	inline const char* const AGENT_NAMES[] = {
		"off",
		"Cmdr. Davida 'Goggles' Fernandez | SEAL Frogman",
		"Cmdr. Frank 'Wet Sox' Baroud | SEAL Frogman",
		"Lieutenant Rex Krikey | SEAL Frogman",
		"Michael Syfers | FBI Sniper",
		"Operator | FBI SWAT",
		"Special Agent Ava | FBI",
		"Markus Delrow | FBI HRT",
		"Sous-Lieutenant Medic | Gendarmerie Nationale",
		"Chem-Haz Capitaine | Gendarmerie Nationale",
		"Chef d'Escadron Rouchard | Gendarmerie Nationale",
		"Aspirant | Gendarmerie Nationale",
		"Officer Jacques Beltram | Gendarmerie Nationale",
		"D Squadron Officer | NZSAS",
		"B Squadron Officer | SAS",
		"Seal Team 6 Soldier | NSWC SEAL",
		"Buckshot | NSWC SEAL",
		"Lt. Commander Ricksaw | NSWC SEAL",
		"'Blueberries' Buckshot | NSWC SEAL",
		"3rd Commando Company | KSK",
		"'Two Times' McCoy | TACP Cavalry",
		"'Two Times' McCoy | USAF TACP",
		"Primeiro Tenente | Brazilian 1st Battalion",
		"Cmdr. Mae 'Dead Cold' Jamison | SWAT",
		"1st Lieutenant Farlow | SWAT",
		"John 'Van Healen' Kask | SWAT",
		"Bio-Haz Specialist | SWAT",
		"Sergeant Bombson | SWAT",
		"Chem-Haz Specialist | SWAT",
		"Lieutenant 'Tree Hugger' Farlow | SWAT",
		"Getaway Sally | The Professionals",
		"Number K | The Professionals",
		"Little Kev | The Professionals",
		"Safecracker Voltzmann | The Professionals",
		"Darryl The Strapped | The Professionals",
		"Sir Loudmouth Darryl | The Professionals",
		"Sir Darryl Royale | The Professionals",
		"Sir Skullhead Darryl | The Professionals",
		"Sir Silent Darryl | The Professionals",
		"Sir Miami Darryl | The Professionals",
		"Street Soldier | Phoenix",
		"Soldier | Phoenix",
		"Slingshot | Phoenix",
		"Enforcer | Phoenix",
		"Mr. Muhlik | Elite Crew",
		"Prof. Shahmat | Elite Crew",
		"Osiris | Elite Crew",
		"Ground Rebel | Elite Crew",
		"The Elite Mr. Muhlik | Elite Crew",
		"Trapper | Guerrilla Warfare",
		"Trapper Aggressor | Guerrilla Warfare",
		"Vypa Sista of the Revolution | Guerrilla Warfare",
		"Col. Mangos Dabisi | Guerrilla Warfare",
		"'Medium Rare' Crasswater | Guerrilla Warfare",
		"Crasswater The Forgotten | Guerrilla Warfare",
		"Elite Trapper Solman | Guerrilla Warfare",
		"'The Doctor' Romanov | Sabre",
		"Blackwolf | Sabre",
		"Maximus | Sabre",
		"Dragomir | Sabre",
		"Rezan The Ready | Sabre",
		"Rezan the Redshirt | Sabre",
		"Dragomir | Sabre Footsoldier",
		"Phoenix | T default",
		"Leet Krew | T default",
		"Separatist | T default",
		"Balkan | T default",
		"Professional | T default",
		"Anarchist | T default",
		"Pirate | T default",
		"SEAL Team 6 | CT default",
		"IDF | CT default",
		"GIGN | CT default",
		"SWAT | CT default",
		"GSG-9 | CT default",
		"SAS | CT default",
		"FBI | CT default",
		"Phoenix (Var A) | T default",
		"Phoenix (Var B) | T default",
		"Phoenix (Var C) | T default",
		"Phoenix (Var D) | T default",
		"Phoenix Heavy | T default",
		"Leet Krew (Var A) | T default",
		"Leet Krew (Var B) | T default",
		"Leet Krew (Var C) | T default",
		"Leet Krew (Var D) | T default",
		"Leet Krew (Var E) | T default",
		"Separatist (Var A) | T default",
		"Separatist (Var B) | T default",
		"Separatist (Var C) | T default",
		"Separatist (Var D) | T default",
		"Balkan (Var A) | T default",
		"Balkan (Var B) | T default",
		"Balkan (Var C) | T default",
		"Balkan (Var D) | T default",
		"Balkan (Var E) | T default",
		"Professional (Var 1) | T default",
		"Professional (Var 2) | T default",
		"Professional (Var 3) | T default",
		"Professional (Var 4) | T default",
		"Anarchist (Var A) | T default",
		"Anarchist (Var B) | T default",
		"Anarchist (Var C) | T default",
		"Anarchist (Var D) | T default",
		"Pirate (Var A) | T default",
		"Pirate (Var B) | T default",
		"Pirate (Var C) | T default",
		"Pirate (Var D) | T default",
		"Jumpsuit (Var A) | T default",
		"Jumpsuit (Var B) | T default",
		"Jumpsuit (Var C) | T default",
		"SEAL Team 6 (Var A) | CT default",
		"SEAL Team 6 (Var B) | CT default",
		"SEAL Team 6 (Var C) | CT default",
		"SEAL Team 6 (Var D) | CT default",
		"IDF (Var B) | CT default",
		"IDF (Var C) | CT default",
		"IDF (Var D) | CT default",
		"IDF (Var E) | CT default",
		"IDF (Var F) | CT default",
		"GIGN (Var A) | CT default",
		"GIGN (Var B) | CT default",
		"GIGN (Var C) | CT default",
		"GIGN (Var D) | CT default",
		"SWAT (Var A) | CT default",
		"SWAT (Var B) | CT default",
		"SWAT (Var C) | CT default",
		"SWAT (Var D) | CT default",
		"GSG-9 (Var A) | CT default",
		"GSG-9 (Var B) | CT default",
		"GSG-9 (Var C) | CT default",
		"GSG-9 (Var D) | CT default",
		"SAS (Var A) | CT default",
		"SAS (Var B) | CT default",
		"SAS (Var C) | CT default",
		"SAS (Var D) | CT default",
		"FBI (Var A) | CT default",
		"FBI (Var C) | CT default",
		"FBI (Var D) | CT default",
		"FBI (Var E) | CT default",
		"Heavy Assault Suit | CT default",
	};
	constexpr int AGENT_NAME_COUNT = sizeof( AGENT_NAMES ) / sizeof( AGENT_NAMES[ 0 ] );

	struct impl_t {
		void on_frame_stage_notify( int stage );

		void on_level_init( );

		void on_level_pre_load( );

		// CBaseViewModel m_nSequence / m_nModelIndex recv proxies. hook on attach, unhook on detach (else crash)
		void animation_hook( );
		void animation_unhook( );

		void publish_anim_donor( );

		void init_parser( );

		void deagle_spinner( );

		/* render_start, before CalcView: zero the daggers' cam_driver so draw anims don't move the camera */
		void fix_dagger_view( );

		void knife_hold( int stage );

		bool m_forcing_update = false;

		std::vector< unsigned int > m_seen_weapons = { };

		struct pending_weapon_t {
			unsigned int m_handle;
			int m_tries;
		};

		std::vector< pending_weapon_t > m_pending_weapons = { };

		// max caps a weapon that can never composite (stale pattern)
		static constexpr int NEW_WEAPON_MIN_TRIES = 2;
		static constexpr int NEW_WEAPON_MAX_TRIES = 8;

		std::vector< paint_kit_entry_t > m_parser_skins  = { };
		std::vector< paint_kit_entry_t > m_parser_gloves = { };
		std::vector< paint_kit_entry_t > m_parser_stickers = { };
		bool m_parser_done                                 = false;

		void dump_model_list( );

	private:
		void agent_changer( );
		void gloves_changer( );
		void knife_changer( );
		bool local_refresh( );
		void full_update( );
	};
}

inline n_skins::impl_t g_skins{ };
