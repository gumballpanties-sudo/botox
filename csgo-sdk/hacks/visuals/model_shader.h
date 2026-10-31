#pragma once

// shared by DIP shader swaps (weapon sheen, lsd chams): render thread only

#include "../../globals/logger/logger.h"

#include <d3d9.h>
#include <d3dx9.h>

#include <cstring>
#include <iterator>
#include <utility>
#include <vector>

namespace n_model_shader
{
	using defs_t = std::vector< std::pair< int, int > >;

	inline ID3DXBuffer* compile( const char* tag, const char* source, const char* profile, const D3DXMACRO* macros )
	{
		ID3DXBuffer *code = nullptr, *errors = nullptr;
		D3DXCompileShader( source, static_cast< UINT >( std::strlen( source ) ), macros, nullptr, "main", profile, 0, &code, &errors, nullptr );

		if ( errors ) {
			botox_dbg_log( "%s: %s compile: %s", tag, profile, static_cast< const char* >( errors->GetBufferPointer( ) ) );
			errors->Release( );
		}

		return code;
	}

	// def literals can leak into the hw register file under the engine's state cache: save + restore each
	inline void collect_defs( const DWORD* code, defs_t& out )
	{
		for ( std::size_t i = 1; code[ i ] != 0x0000FFFF; ) {
			const DWORD token  = code[ i ];
			const DWORD opcode = token & 0xFFFF;

			if ( opcode == D3DSIO_COMMENT ) {
				i += 1 + ( ( token & D3DSI_COMMENTSIZE_MASK ) >> D3DSI_COMMENTSIZE_SHIFT );
				continue;
			}

			if ( opcode == D3DSIO_DEF || opcode == D3DSIO_DEFI || opcode == D3DSIO_DEFB )
				out.emplace_back( opcode == D3DSIO_DEF ? 0 : opcode == D3DSIO_DEFI ? 1 : 2, static_cast< int >( code[ i + 1 ] & D3DSP_REGNUM_MASK ) );

			i += 1 + ( ( token & D3DSI_INSTLENGTH_MASK ) >> D3DSI_INSTLENGTH_SHIFT );
		}
	}

	// -1 = not a studio decl we can read, 0 = float normals, 1 = csgo UBYTE4 compressed (vertexdecl.cpp:142)
	inline int vertex_compression( IDirect3DDevice9* device )
	{
		static IDirect3DVertexDeclaration9* last = nullptr;
		static int result                        = -1;

		IDirect3DVertexDeclaration9* declaration = nullptr;
		if ( FAILED( device->GetVertexDeclaration( &declaration ) ) || !declaration )
			return -1;

		if ( declaration != last ) {
			D3DVERTEXELEMENT9 elements[ MAXD3DDECLLENGTH + 1 ]{ };
			UINT count = 0;
			result     = -1;

			if ( SUCCEEDED( declaration->GetDeclaration( nullptr, &count ) ) && count <= std::size( elements ) &&
			     SUCCEEDED( declaration->GetDeclaration( elements, &count ) ) ) {
				for ( UINT i = 0; i < count && elements[ i ].Stream != 0xFF; i++ ) {
					if ( elements[ i ].Usage == D3DDECLUSAGE_NORMAL && elements[ i ].UsageIndex == 0 )
						result = elements[ i ].Type == D3DDECLTYPE_UBYTE4 ? 1 : elements[ i ].Type == D3DDECLTYPE_FLOAT3 ? 0 : -1;
				}
			}

			last = declaration;
		}

		declaration->Release( );
		return result;
	}

	struct saved_constant_t {
		int m_kind;
		int m_register;
		float m_value[ 4 ];
	};

	inline void save_constants( IDirect3DDevice9* device, const bool vertex, const defs_t& defs, std::vector< saved_constant_t >& out )
	{
		for ( const auto& [ kind, reg ] : defs ) {
			saved_constant_t saved{ kind, reg, { } };

			if ( kind == 0 )
				vertex ? device->GetVertexShaderConstantF( reg, saved.m_value, 1 ) : device->GetPixelShaderConstantF( reg, saved.m_value, 1 );
			else if ( kind == 1 )
				vertex ? device->GetVertexShaderConstantI( reg, reinterpret_cast< int* >( saved.m_value ), 1 )
				       : device->GetPixelShaderConstantI( reg, reinterpret_cast< int* >( saved.m_value ), 1 );
			else
				vertex ? device->GetVertexShaderConstantB( reg, reinterpret_cast< BOOL* >( saved.m_value ), 1 )
				       : device->GetPixelShaderConstantB( reg, reinterpret_cast< BOOL* >( saved.m_value ), 1 );

			out.push_back( saved );
		}
	}

	inline void restore_constants( IDirect3DDevice9* device, const bool vertex, const std::vector< saved_constant_t >& saved )
	{
		for ( const saved_constant_t& entry : saved ) {
			if ( entry.m_kind == 0 )
				vertex ? device->SetVertexShaderConstantF( entry.m_register, entry.m_value, 1 ) : device->SetPixelShaderConstantF( entry.m_register, entry.m_value, 1 );
			else if ( entry.m_kind == 1 )
				vertex ? device->SetVertexShaderConstantI( entry.m_register, reinterpret_cast< const int* >( entry.m_value ), 1 )
				       : device->SetPixelShaderConstantI( entry.m_register, reinterpret_cast< const int* >( entry.m_value ), 1 );
			else
				vertex ? device->SetVertexShaderConstantB( entry.m_register, reinterpret_cast< const BOOL* >( entry.m_value ), 1 )
				       : device->SetPixelShaderConstantB( entry.m_register, reinterpret_cast< const BOOL* >( entry.m_value ), 1 );
		}
	}
}
