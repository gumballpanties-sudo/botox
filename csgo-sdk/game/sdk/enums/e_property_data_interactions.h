#pragma once

enum e_property_data_interactions : int {
	propinter_physgun_world_stick,      
	propinter_physgun_first_break,      
	propinter_physgun_first_paint,      
	propinter_physgun_first_impale,     
	propinter_physgun_launch_spin_none, 
	propinter_physgun_launch_spin_z,    
	propinter_physgun_break_explode,    
	propinter_physgun_break_explode_ice,
	propinter_physgun_damage_none,      
	propinter_fire_flammable,           
	propinter_fire_explosive_resist,    
	propinter_fire_ignite_halfhealth,   
	propinter_physgun_create_flare,     
	propinter_physgun_allow_overhead,   
	propinter_world_bloodsplat,         
	propinter_physgun_notify_children,  
	propinter_melee_immune,             
	propinter_num_interactions          
};