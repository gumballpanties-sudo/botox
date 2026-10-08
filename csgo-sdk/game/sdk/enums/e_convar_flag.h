#pragma once

enum e_convar_flag : int {
	// convar systems
	fcvar_none            = 0,
	fcvar_unregistered    = ( 1 << 0 ), // if this is set, don't add to linked list, etc.
	fcvar_developmentonly = ( 1 << 1 ),
	fcvar_gamedll         = ( 1 << 2 ), // defined by the game dll
	fcvar_clientdll       = ( 1 << 3 ), // defined by the client dll
	fcvar_hidden          = ( 1 << 4 ), // hidden. doesn't appear in find or autocomplete. like developmentonly, but can't be compiled out.

	// convar only
	fcvar_protected =
		( 1
	      << 5 ), // it's a server cvar, but we don't send the data since it's a password, etc.  sends 1 if it's not bland/zero, 0 otherwise as value
	fcvar_sponly          = ( 1 << 6 ),  // this cvar cannot be changed by clients connected to a multiplayer server.
	fcvar_archive         = ( 1 << 7 ),  // set to cause it to be saved to vars.rc
	fcvar_notify          = ( 1 << 8 ),  // notifies players when changed
	fcvar_userinfo        = ( 1 << 9 ),  // changes the client's info string
	fcvar_cheat           = ( 1 << 14 ), // only useable in singleplayer / debug / multiplayer & sv_cheats
	fcvar_printableonly   = ( 1 << 10 ),
	fcvar_unlogged        = ( 1 << 11 ), // if this is a fcvar_server, don't log changes to the log file / console if we are creating a log
	fcvar_never_as_string = ( 1 << 12 ), // never try to print that cvar

	fcvar_server                  = ( 1 << 13 ), // server setting enforced on clients, replicated
	fcvar_demo                    = ( 1 << 16 ), // record this cvar when starting a demo file
	fcvar_dontrecord              = ( 1 << 17 ), // don't record these command in demofiles
	fcvar_reload_materials        = ( 1 << 20 ), // if this cvar changes, it forces a material reload
	fcvar_reload_textures         = ( 1 << 21 ), // if this cvar changes, if forces a texture reload
	fcvar_not_connected           = ( 1 << 22 ), // cvar cannot be changed by a client that is connected to a server
	fcvar_material_system_thread  = ( 1 << 23 ), // indicates this cvar is read from the material system thread
	fcvar_archive_xbox            = ( 1 << 24 ), // cvar written to config.cfg on the xbox
	fcvar_accessible_from_threads = ( 1 << 25 ), // used as a debugging tool necessary to check material system thread convars
	fcvar_server_can_execute =
		( 1 << 28 ), // the server is allowed to execute this command on clients via clientcommand/net_stringcmd/cbaseclientstate::processstringcmd.
	fcvar_server_cannot_query =
		( 1 << 29 ),
	fcvar_clientcmd_can_execute = ( 1 << 30 ), // ivengineclient::clientcmd is allowed to execute this command.
	fcvar_material_thread_mask  = ( fcvar_reload_materials | fcvar_reload_textures | fcvar_material_system_thread )
};