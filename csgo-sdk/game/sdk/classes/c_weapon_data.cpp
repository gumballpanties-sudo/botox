#include "c_weapon_data.h"
#include "../enums/e_weapon_type.h"

bool c_weapon_data::is_gun( ) const
{
	switch ( this->m_weapon_type ) {
	case e_weapon_type::weapontype_pistol:
	case e_weapon_type::weapontype_submachinegun:
	case e_weapon_type::weapontype_rifle:
	case e_weapon_type::weapontype_shotgun:
	case e_weapon_type::weapontype_sniper:
	case e_weapon_type::weapontype_machinegun:
		return true;
	}

	return false;
}