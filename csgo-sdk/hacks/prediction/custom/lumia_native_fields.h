#pragma once
#include "../../../game/sdk/structs/data_map_t.h"
#include "../../../game/sdk/enums/e_field_types.h"
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace n_lumia_native {
    // Matches the installed Win32 client's 60-byte typedescription_t, rather
    // than the shared SDK's two-offset declaration. Verified by prior native
    // datamap snapshot tests; no offsets are guessed from a player address.
    class native_fields_t {
        struct description_t {
            e_field_types type;
            const char* name;
            int offset;
            unsigned short count;
            short flags;
            unsigned char metadata[12];
            data_map_t* embedded;
            unsigned char tail[28];
        };
        static_assert(sizeof(description_t)==60&&offsetof(description_t,count)==12&&offsetof(description_t,embedded)==28);
        struct field_t {const char* name;std::string qualified;int offset,bytes;};
        std::vector<field_t> fields;
        data_map_t* root=nullptr;
        static int width(e_field_types type) {
            switch(type) {
            case field_boolean:case field_character:return 1;
            case field_short:return 2;
            case field_float:case field_integer:case field_ehandle:case field_time:case field_tick:
            case field_color32:case field_modelindex:case field_materialindex:return 4;
            case field_vector2d:case field_integer64:return 8;
            case field_vector:case field_position_vector:return 12;
            case field_quaternion:case field_vector4d:return 16;
            default:return 0;
            }
        }
        void visit(data_map_t* map,int parent=0,int depth=0,const std::string& prefix={}) {
            if(!map||depth>12||map->m_data_fields<0||map->m_data_fields>2048) return;
            if(map->m_data_fields&&!map->m_data_desc) return;
            for(int i=0;i<map->m_data_fields;++i) {
                description_t description{};
                std::memcpy(&description,reinterpret_cast<const unsigned char*>(map->m_data_desc)+i*sizeof(description),sizeof(description));
                // Published CCSPlayer fields extend beyond 64 KiB in build
                // 8802 (e.g. m_bDuckOverride). The native object is not a 64K
                // blob; cap malformed offsets without excluding real fields.
                if(description.offset<0||description.offset>0x40000-parent) continue;
                const int offset=parent+description.offset;
                if(description.type==field_embedded&&description.count==1&&description.name) visit(description.embedded,offset,depth+1,prefix+description.name+".");
                const int bytes=width(description.type)*description.count;
                if(!description.name||offset<=0||bytes<=0||bytes>4096) continue;
                fields.push_back({description.name,prefix+description.name,offset,bytes});
            }
            visit(map->m_base_map,parent,depth+1,prefix);
        }
    public:
        struct snapshot_t {
            const data_map_t* identity=nullptr;
            std::vector<unsigned char> bytes;
        };
        bool bind(data_map_t* map) {
            if(map!=root) {fields.clear();root=map;visit(map);}
            return !fields.empty();
        }
        int offset_of(const char* name,int bytes) const {
            if(!name||bytes<=0) return 0;
            int offset=0;
            const bool qualified=std::strchr(name,'.')!=nullptr;
            for(const auto& field:fields) if(field.bytes==bytes&&(qualified?field.qualified==name:std::strcmp(field.name,name)==0)) {
                if(offset&&offset!=field.offset) return 0;
                offset=field.offset;
            }
            return offset;
        }
        template<class T> bool read(const void* entity,const char* name,T& out) const {
            if(!entity) return false;
            const int offset=offset_of(name,sizeof(T));if(!offset) return false;
            std::memcpy(&out,reinterpret_cast<const unsigned char*>(entity)+offset,sizeof(T));return true;
        }
        snapshot_t save_values(const void* entity)const {
            snapshot_t out;out.identity=root;if(!entity)return out;
            std::size_t size=0;for(const auto& field:fields)size+=field.bytes;
            out.bytes.resize(size);std::size_t at=0;
            for(const auto& field:fields) {
                std::memcpy(out.bytes.data()+at,reinterpret_cast<const unsigned char*>(entity)+field.offset,field.bytes);at+=field.bytes;
            }
            return out;
        }
        bool restore_values(void* entity,const snapshot_t& saved)const {
            if(!entity||saved.identity!=root)return false;
            std::size_t size=0;for(const auto& field:fields)size+=field.bytes;
            if(saved.bytes.size()!=size)return false;
            std::size_t at=0;
            for(const auto& field:fields) {
                std::memcpy(reinterpret_cast<unsigned char*>(entity)+field.offset,saved.bytes.data()+at,field.bytes);at+=field.bytes;
            }
            return true;
        }
    };
}
