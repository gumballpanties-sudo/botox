#pragma once

class c_weapon_data
{
public:
	unsigned char pad0[ 0x14 ];              
	int m_max_clip1;                         
	int m_max_clip2;                         
	int m_default_clip1;                     
	int m_default_clip2;                     
	int m_primary_max_reserve_ammo;          
	int m_secondary_max_reserve_ammo;        
	const char* m_world_model;              
	const char* m_view_model;               
	const char* m_dropped_model;            
	unsigned char pad1[ 0x50 ];              
	const char* m_hud_name;                 
	const char* m_weapon_name;              
	unsigned char pad2[ 0x2 ];               
	bool m_is_melee_weapon;                  
	unsigned char pad3[ 0x9 ];               
	float m_weapon_weight;                  
	unsigned char pad4[ 0x4 ];               
	int m_slot;                              
	int m_position;                          
	unsigned char pad5[ 0x1c ];              
	int m_weapon_type;                       
	unsigned char pad6[ 0x4 ];               
	int m_weapon_price;                      
	int m_kill_award;                        
	const char* m_animation_prefix;         
	float m_cycle_time;                     
	float m_cycle_time_alt;                 
	unsigned char pad8[ 0x8 ];               
	bool m_full_auto;                        
	unsigned char pad9[ 0x3 ];               
	int m_damage;                            
	float m_head_shot_multiplier;           
	float m_armor_ratio;                    
	int m_bullets;                           
	float m_penetration;                    
	unsigned char pad10[ 0x8 ];              
	float m_range;                          
	float m_range_modifier;                 
	float m_throw_velocity;                 
	unsigned char pad11[ 0xc ];              
	bool m_has_silencer;                     
	unsigned char pad12[ 0xf ];              
	float m_max_speed[ 2 ];                 
	unsigned char pad13[ 0x4 ];              
	float m_spread[ 2 ];                    
	float m_inaccuracy_crouch[ 2 ];         
	float m_inaccuracy_stand[ 2 ];          
	unsigned char pad14[ 0x8 ];              
	float m_inaccuracy_jump[ 2 ];           
	float m_inaccuracy_land[ 2 ];           
	float m_inaccuracy_ladder[ 2 ];         
	float m_inaccuracy_fire[ 2 ];           
	float m_inaccuracy_move[ 2 ];           
	float m_inaccuracy_reload;              
	int m_recoil_seed;                       
	float m_recoil_angle[ 2 ];              
	float m_recoil_angle_variance[ 2 ];     
	float m_recoil_magnitude[ 2 ];          
	float m_recoil_magnitude_variance[ 2 ]; 
	int m_spread_seed;                       

	bool is_gun( ) const;
};