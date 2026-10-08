#pragma once

enum e_collision_group : int {
	collision_group_none = 0,
	collision_group_debris,             
	collision_group_debris_trigger,     
	collision_group_interactive_debris, 
	collision_group_interactive,        
	collision_group_player,
	collision_group_breakable_glass,
	collision_group_vehicle,
	collision_group_player_movement,
	collision_group_npc,            
	collision_group_in_vehicle,     
	collision_group_weapon,         
	collision_group_vehicle_clip,   
	collision_group_projectile,     
	collision_group_door_blocker,   
	collision_group_passable_door,  
	collision_group_dissolving,     
	collision_group_pushaway,       
	collision_group_npc_actor,      
	collision_group_npc_scripted,   
	collision_group_pz_clip,
	collision_group_debris_block_projectile, 
	last_shared_collision_group
};