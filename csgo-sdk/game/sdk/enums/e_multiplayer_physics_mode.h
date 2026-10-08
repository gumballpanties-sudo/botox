#pragma once

enum e_multiplayer_physics_mode : int {
	physics_multiplayer_autodetect = 0,
	physics_multiplayer_solid      = 1,
	physics_multiplayer_non_solid  = 2,
	physics_multiplayer_clientside = 3 
};