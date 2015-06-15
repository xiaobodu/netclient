#include "LuaPacket.h"
#include "assert.h"
#include "RPacket.h"
#include "WPacket.h"
#include "CmdRPacket.h"
#include "CmdWPacket.h"

enum{
	L_TABLE = 1,
	L_STRING,
	L_BOOL,
	L_FLOAT,
	L_UINT8,
	L_UINT16,
	L_UINT32,
	L_UINT64,
	L_INT8,
	L_INT16,
	L_INT32,
	L_INT64,
};

typedef struct{
	 net::Packet* packet;
}lua_packet,*lua_packet_t;

#define LUARPACKET_METATABLE    "luarpacket_metatable"
#define LUAWPACKET_METATABLE    "luawpacket_metatable"
#define LUACMDRPACKET_METATABLE "luacmdrpacket_metatable"
#define LUACMDWPACKET_METATABLE "luacmdwpacket_metatable"

#define VAILD_KEY_TYPE(TYPE) (TYPE == LUA_TSTRING || TYPE == LUA_TNUMBER)
#define VAILD_VAILD_TYPE(TYPE) (TYPE == LUA_TSTRING || TYPE == LUA_TNUMBER || TYPE == LUA_TTABLE || TYPE == LUA_TBOOLEAN)

static inline void luabin_pack_string(net::StreamWPacket* wpk,lua_State *L,int index){
	wpk->WriteUint8(L_STRING);
	size_t len;
	const char *data = lua_tolstring(L,index,&len);
	wpk->WriteBin((void*)data, len);
}

static inline void luabin_pack_number(net::StreamWPacket* wpk,lua_State *L,int index){
	lua_Number v = lua_tonumber(L,index);
	if(v != (lua_Integer)v){
		wpk->WriteUint8(L_FLOAT);
		wpk->WriteDouble(v);
	}else{
		if((long long)v > 0){
			unsigned long long _v = (unsigned long long)v;
			if(_v <= 0xFF){
				wpk->WriteUint8(L_UINT8);
				wpk->WriteUint8((unsigned char)_v);
			}else if(_v <= 0xFFFF){
				wpk->WriteUint8(L_UINT16);
				wpk->WriteUint16((unsigned short)_v);
			}else if(_v <= 0xFFFFFFFF){
				wpk->WriteUint8(L_UINT32);
				wpk->WriteUint32((unsigned int)_v);
			}else{
				wpk->WriteUint8(L_UINT64);
				wpk->WriteUint64((unsigned long long)_v);
			}
		}else{
			long long _v = (long long)v;
			if(_v >= 0x80){
				wpk->WriteUint8(L_INT8);
				wpk->WriteUint8((unsigned char)_v);
			}else if(_v >= 0x8000){
				wpk->WriteUint8(L_INT16);
				wpk->WriteUint16((unsigned short)_v);
			}else if(_v < 0x80000000){
				wpk->WriteUint8(L_INT32);
				wpk->WriteUint32((unsigned int)_v);
			}else{
				wpk->WriteUint8(L_INT64);
				wpk->WriteUint64((unsigned long long)_v);
			}
		}
	}
}

static inline void luabin_pack_boolean(net::StreamWPacket* wpk,lua_State *L,int index){
	wpk->WriteUint8(L_BOOL);
	int value = lua_toboolean(L,index);
	wpk->WriteUint8(value);
}


static int luabin_pack_table(net::StreamWPacket* wpk,lua_State *L,int index){
	if(0 != lua_getmetatable(L,index)){
		lua_pop(L,1);
		return -1;
	}
	wpk->WriteUint8(L_TABLE);
	net::write_pos wpos = wpk->GetWritePos();
	wpk->WriteUint32(0);
	int ret = 0;
	int c = 0;
	int top = lua_gettop(L);
	lua_pushnil(L);
	do{		
		if(!lua_next(L,index - 1)){
			break;
		}
		int key_type = lua_type(L, -2);
		int val_type = lua_type(L, -1);
		if(!VAILD_KEY_TYPE(key_type)){
			lua_pop(L,1);
			continue;
		}
		if(!VAILD_VAILD_TYPE(val_type)){
			lua_pop(L,1);
			continue;
		}
		if(key_type == LUA_TSTRING)
			luabin_pack_string(wpk,L,-2);
		else
			luabin_pack_number(wpk,L,-2);

		if(val_type == LUA_TSTRING)
			luabin_pack_string(wpk,L,-1);
		else if(val_type == LUA_TNUMBER)
			luabin_pack_number(wpk,L,-1);
		else if(val_type == LUA_TBOOLEAN)
			luabin_pack_boolean(wpk,L,-1);
		else if(val_type == LUA_TTABLE){
				if(0 != (ret = luabin_pack_table(wpk,L,-1)))
					break;
		}else{
			ret = -1;
			break;
		}
		lua_pop(L,1);
		++c;
	}while(1);
	lua_settop(L,top);
	if(0 == ret){
		wpk->RewriteUint32(wpos, c);
	}						
	return ret;
}

static inline void un_pack_boolean(net::StreamRPacket *rpk,lua_State *L){
	int n = rpk->ReadUint8();
	lua_pushboolean(L,n);
}

static inline void un_pack_number(net::StreamRPacket *rpk,lua_State *L,int type){
	double n;// = rpk_read_double(rpk);
	switch(type){
		case L_FLOAT:{
			n = rpk->ReadDouble();
			break;
		}
		case L_UINT8:{
			n = (double)rpk->ReadUint8();
			break;
		}
		case L_UINT16:{
			n = (double)rpk->ReadUint16();
			break;
		}
		case L_UINT32:{
			n = (double)rpk->ReadUint32();
			break;
		}
		case L_UINT64:{
			n = (double)rpk->ReadUint64();
			break;
		}
		case L_INT8:{
			n = (double)((char)rpk->ReadUint8());
			break;
		}
		case L_INT16:{
			n = (double)((short)rpk->ReadUint16());
			break;
		}
		case L_INT32:{
			n = (double)((int)rpk->ReadUint32());
			break;
		}
		case L_INT64:{
			n = (double)((long long)rpk->ReadUint64());
			break;
		}
		default:{
			assert(0);
			break;
		}
	}
	lua_pushnumber(L,n);
}

static inline void un_pack_string(net::StreamRPacket *rpk,lua_State *L){
	size_t len;
	const char *data = (const char*)rpk->ReadBin(len);
	lua_pushlstring(L,data,(size_t)len);
}


static int un_pack_table(net::StreamRPacket *rpk,lua_State *L){
	int size = rpk->ReadUint32();
	int i = 0;
	lua_newtable(L);
	for(; i < size; ++i){
		int key_type,value_type;
		key_type = rpk->ReadUint8();
		if(key_type == L_STRING){
			un_pack_string(rpk,L);
		}else if(key_type >= L_FLOAT && key_type <= L_INT64){
			un_pack_number(rpk,L,key_type);
		}else
			return -1;
		value_type = rpk->ReadUint8();
		if(value_type == L_STRING){
			un_pack_string(rpk,L);
		}else if(value_type >= L_FLOAT && value_type <= L_INT64){
			un_pack_number(rpk,L,value_type);
		}else if(value_type == L_BOOL){
			un_pack_boolean(rpk,L);
		}else if(value_type == L_TABLE){
			if(0 != un_pack_table(rpk,L)){
				return -1;
			}
		}else
			return -1;
		lua_rawset(L,-3);
	}
	return 0;
}

inline static lua_packet_t lua_getluapacket(lua_State *L, int index) {
	return (lua_packet_t)lua_touserdata(L,index);//luaL_checkudata(L, index, LUARPACKET_METATABLE);
}


static int ReadUint8(lua_State *L){
	lua_packet_t p = lua_getluapacket(L,1);
	if (!p || !p->packet) return luaL_error(L,"invaild opration");
	net::StreamRPacket *rpk = dynamic_cast<net::StreamRPacket*>(p->packet);
	if(!rpk) return luaL_error(L,"invaild opration");
	lua_pushinteger(L,(lua_Integer)rpk->ReadUint8());
	return 1;
}

static int ReadUint16(lua_State *L){
	lua_packet_t p = lua_getluapacket(L,1);
	if (!p || !p->packet) return luaL_error(L,"invaild opration");
	net::StreamRPacket *rpk = dynamic_cast<net::StreamRPacket*>(p->packet);
	if(!rpk) return luaL_error(L,"invaild opration");
	lua_pushinteger(L,(lua_Integer)rpk->ReadUint16());
	return 1;
}

static int ReadUint32(lua_State *L){
	lua_packet_t p = lua_getluapacket(L,1);
	if (!p || !p->packet) return luaL_error(L,"invaild opration");
	net::StreamRPacket *rpk = dynamic_cast<net::StreamRPacket*>(p->packet);
	if(!rpk) return luaL_error(L,"invaild opration");
	lua_pushinteger(L,(lua_Integer)rpk->ReadUint32());	
	return 1;
}

static int ReadInt32(lua_State *L){
	lua_packet_t p = lua_getluapacket(L,1);
	if (!p || !p->packet) return luaL_error(L,"invaild opration");
	net::StreamRPacket *rpk = dynamic_cast<net::StreamRPacket*>(p->packet);
	if(!rpk) return luaL_error(L,"invaild opration");
	lua_pushinteger(L, (lua_Integer)rpk->ReadInt32());
	return 1;
}

static int ReadDouble(lua_State *L){
	lua_packet_t p = lua_getluapacket(L,1);
	if (!p || !p->packet) return luaL_error(L,"invaild opration");
	net::StreamRPacket *rpk = dynamic_cast<net::StreamRPacket*>(p->packet);
	if(!rpk) return luaL_error(L,"invaild opration");
	lua_pushnumber(L,(lua_Number)rpk->ReadDouble());	
	return 1;
}

static int ReadString(lua_State *L){
	lua_packet_t p = lua_getluapacket(L,1);
	if (!p || !p->packet) return luaL_error(L,"invaild opration");
	net::StreamRPacket *rpk = dynamic_cast<net::StreamRPacket*>(p->packet);
	if(!rpk) return luaL_error(L,"invaild opration");
	size_t len;
	const char* str = (const char*)rpk->ReadBin(len);
	if(str)
		lua_pushlstring(L,str,len);
	else
		lua_pushnil(L);
	return 1;
}

static int ReadTable(lua_State *L) {
	lua_packet_t p = lua_getluapacket(L,1);
	if (!p || !p->packet) return luaL_error(L,"invaild opration");
	net::StreamRPacket *rpk = dynamic_cast<net::StreamRPacket*>(p->packet);
	if(!rpk) return luaL_error(L,"invaild opration");
	int type = rpk->ReadUint8();
	if(type != L_TABLE){
		lua_pushnil(L);
		return 1;
	}
	int old_top = lua_gettop(L);
	int ret = un_pack_table(rpk,L);
	if(0 != ret){
		lua_settop(L,old_top);
		lua_pushnil(L);
	}
	return 1;
}

static int WriteUint8(lua_State *L){
	lua_packet_t p = lua_getluapacket(L,1);
	if (!p || !p->packet)return luaL_error(L,"invaild opration");
	net::StreamWPacket *wpk = dynamic_cast<net::StreamWPacket*>(p->packet);
	if(!wpk)return luaL_error(L,"invaild opration");	
	if(lua_type(L,2) != LUA_TNUMBER)
		return luaL_error(L,"invaild arg2");
	unsigned char value = (unsigned char)lua_tointeger(L, 2);
	wpk->WriteUint8(value);
	return 0;
}

static int WriteUint16(lua_State *L){
	lua_packet_t p = lua_getluapacket(L,1);
	if (!p || !p->packet)return luaL_error(L,"invaild opration");
	net::StreamWPacket *wpk = dynamic_cast<net::StreamWPacket*>(p->packet);
	if(!wpk)return luaL_error(L,"invaild opration");	
	if(lua_type(L,2) != LUA_TNUMBER)
		return luaL_error(L,"invaild arg2");
	unsigned short value = (unsigned short)lua_tointeger(L, 2);
	wpk->WriteUint16(value);
	return 0;
}

static int WriteUint32(lua_State *L){
	lua_packet_t p = lua_getluapacket(L,1);
	if (!p || !p->packet)return luaL_error(L,"invaild opration");
	net::StreamWPacket *wpk = dynamic_cast<net::StreamWPacket*>(p->packet);
	if(!wpk)return luaL_error(L,"invaild opration");	
	if(lua_type(L,2) != LUA_TNUMBER)
		return luaL_error(L,"invaild arg2");
	unsigned int value = (unsigned int)lua_tointeger(L, 2);
	wpk->WriteUint32(value);
	return 0;
}

static int WriteDouble(lua_State *L){
	lua_packet_t p = lua_getluapacket(L,1);
	if (!p || !p->packet)return luaL_error(L,"invaild opration");
	net::StreamWPacket *wpk = dynamic_cast<net::StreamWPacket*>(p->packet);
	if(!wpk)return luaL_error(L,"invaild opration");	
	if(lua_type(L,2) != LUA_TNUMBER)
		return luaL_error(L,"invaild arg2");
	double value = (double)lua_tonumber(L, 2);
	wpk->WriteDouble(value);
	return 0;
}

static int WriteString(lua_State *L){
	lua_packet_t p = lua_getluapacket(L,1);
	if (!p || !p->packet)return luaL_error(L,"invaild opration");
	net::StreamWPacket *wpk = dynamic_cast<net::StreamWPacket*>(p->packet);
	if(!wpk)return luaL_error(L,"invaild opration");	
	if(lua_type(L,2) != LUA_TSTRING)
		return luaL_error(L,"invaild arg2");
	size_t len;
	const char *val = lua_tolstring(L, 2, &len);
	wpk->WriteBin((void*)val, len);
	return 0;
}

static int WriteTable(lua_State *L){
	lua_packet_t p = lua_getluapacket(L,1);
	if (!p || !p->packet)return luaL_error(L,"invaild opration");
	net::StreamWPacket *wpk = dynamic_cast<net::StreamWPacket*>(p->packet);
	if(!wpk)return luaL_error(L,"invaild opration");	
	if(LUA_TTABLE != lua_type(L, 2))
		return luaL_error(L,"argument should be lua table");
	if(0 != luabin_pack_table(wpk,L,-1))
		return luaL_error(L,"table should not hava metatable");	
	return 0;	
}

static int RewriteUint8(lua_State *L) {
	lua_packet_t p = lua_getluapacket(L,1);
	if (!p || !p->packet)return luaL_error(L,"invaild opration");
	net::StreamWPacket *wpk = dynamic_cast<net::StreamWPacket*>(p->packet);
	if(!wpk)return luaL_error(L,"invaild opration");
	if(lua_type(L,2) != LUA_TNUMBER)
		return luaL_error(L,"invaild arg2");
	if(lua_type(L,3) != LUA_TNUMBER)
		return luaL_error(L,"invaild arg3");
	net::write_pos wpos = (net::write_pos)lua_tointeger(L,2);
	unsigned char value = (unsigned char)lua_tointeger(L, 3);
	wpk->RewriteUint8(wpos, value);
	return 0;
}

static int RewriteUint16(lua_State *L) {
	lua_packet_t p = lua_getluapacket(L,1);
	if (!p || !p->packet)return luaL_error(L,"invaild opration");
	net::StreamWPacket *wpk = dynamic_cast<net::StreamWPacket*>(p->packet);
	if(!wpk)return luaL_error(L,"invaild opration");
	if(lua_type(L,2) != LUA_TNUMBER)
		return luaL_error(L,"invaild arg2");
	if(lua_type(L,3) != LUA_TNUMBER)
		return luaL_error(L,"invaild arg3");
	net::write_pos wpos = (net::write_pos)lua_tointeger(L,2);
	unsigned short value = (unsigned short)lua_tointeger(L, 3);
	wpk->RewriteUint16(wpos, value);
	return 0;
}

static int RewriteUint32(lua_State *L) {
	lua_packet_t p = lua_getluapacket(L,1);
	if (!p || !p->packet)return luaL_error(L,"invaild opration");
	net::StreamWPacket *wpk = dynamic_cast<net::StreamWPacket*>(p->packet);
	if(!wpk)return luaL_error(L,"invaild opration");
	if(lua_type(L,2) != LUA_TNUMBER)
		return luaL_error(L,"invaild arg2");
	if(lua_type(L,3) != LUA_TNUMBER)
		return luaL_error(L,"invaild arg3");
	net::write_pos wpos = (net::write_pos)lua_tointeger(L,2);
	unsigned int value = (unsigned int)lua_tointeger(L, 3);
	wpk->RewriteUint32(wpos, value);
	return 0;
}

static int RewriteDouble(lua_State *L) {
	lua_packet_t p = lua_getluapacket(L,1);
	if (!p || !p->packet)return luaL_error(L,"invaild opration");
	net::StreamWPacket *wpk = dynamic_cast<net::StreamWPacket*>(p->packet);
	if(!wpk)return luaL_error(L,"invaild opration");
	if(lua_type(L,2) != LUA_TNUMBER)
		return luaL_error(L,"invaild arg2");
	if(lua_type(L,3) != LUA_TNUMBER)
		return luaL_error(L,"invaild arg3");
	net::write_pos wpos = (net::write_pos)lua_tointeger(L,2);
	double value = (double)lua_tonumber(L, 3);
	wpk->RewriteDouble(wpos, value);
	return 0;
}

static int GetWritePos(lua_State *L) {
	lua_packet_t p = lua_getluapacket(L,1);
	if (!p || !p->packet)return luaL_error(L,"invaild opration");
	net::StreamWPacket *wpk = dynamic_cast<net::StreamWPacket*>(p->packet);
	if(!wpk)return luaL_error(L,"invaild opration");
	lua_pushinteger(L, (lua_Integer)wpk->GetWritePos());
	return 1;
}

static int NewWPacket(lua_State *L){
	int argtype = lua_type(L,1); 
	if(argtype == LUA_TNUMBER || argtype == LUA_TNIL || argtype == LUA_TNONE){
		size_t len = 0;
		if(argtype == LUA_TNUMBER) {
			len = lua_tointeger(L,1);
		}
		if(len < 64) {
			len = 64;
		}
		lua_packet_t p = (lua_packet_t)lua_newuserdata(L, sizeof(*p));
		luaL_getmetatable(L, LUAWPACKET_METATABLE);
		lua_setmetatable(L, -2);
		p->packet = new net::WPacket(len);
		return 1;
	} else if(argtype ==  LUA_TUSERDATA) {
		lua_packet_t o = lua_getluapacket(L,1);
		if(!o || !o->packet || o->packet->Type() != RPACKET) {
			return luaL_error(L,"invaild opration for arg1");
		}
		lua_packet_t p = (lua_packet_t)lua_newuserdata(L, sizeof(*p));
		luaL_getmetatable(L, LUAWPACKET_METATABLE);
		lua_setmetatable(L, -2);
		p->packet = new net::WPacket(*dynamic_cast<net::RPacket*>(o->packet));
		return 1;
	} else if(argtype == LUA_TTABLE) {
		net::WPacket* wpk = new net::WPacket(512);
		if(0 != luabin_pack_table(wpk,L,-1)){
			delete wpk;
			return luaL_error(L,"table should not hava metatable");	
		}else{
			lua_packet_t p = (lua_packet_t)lua_newuserdata(L, sizeof(*p));
			luaL_getmetatable(L, LUAWPACKET_METATABLE);
			lua_setmetatable(L, -2);
			p->packet = wpk;
		}
		return 1;
	} else {
		return luaL_error(L,"invaild opration for arg1");
	}
}

static int NewRPacket(lua_State *L){
	if (lua_type(L,1) == LUA_TUSERDATA) {
		lua_packet_t o = lua_getluapacket(L,1);
		if(!o || !o->packet || o->packet->Type() != RPACKET) {
			return luaL_error(L,"invaild opration for arg1");
		}
		lua_packet_t p = (lua_packet_t)lua_newuserdata(L, sizeof(*p));
		luaL_getmetatable(L, LUARPACKET_METATABLE);
		lua_setmetatable(L, -2);
		p->packet = new net::RPacket(*dynamic_cast<net::RPacket*>(o->packet));
		return 1;
	} else {
		return luaL_error(L,"invaild opration for arg1");
	}
}

static int NewCmdWPacket(lua_State *L){
	int argtype = lua_type(L,1); 
	if(argtype == LUA_TNUMBER || argtype == LUA_TNIL || argtype == LUA_TNONE){
		size_t len = 0;
		if(argtype == LUA_TNUMBER) {
			len = lua_tointeger(L,1);
		}
		if(len < 64) {
			len = 64;
		}
		lua_packet_t p = (lua_packet_t)lua_newuserdata(L, sizeof(*p));
		luaL_getmetatable(L, LUAWPACKET_METATABLE);
		lua_setmetatable(L, -2);
		p->packet = new net::CmdWPacket(len);
		return 1;
	} else if(argtype ==  LUA_TUSERDATA) {
		lua_packet_t o = lua_getluapacket(L,1);
		if(!o || !o->packet || o->packet->Type() != CMDRPACKET) {
			return luaL_error(L,"invaild opration for arg1");
		}
		lua_packet_t p = (lua_packet_t)lua_newuserdata(L, sizeof(*p));
		luaL_getmetatable(L, LUAWPACKET_METATABLE);
		lua_setmetatable(L, -2);
		p->packet = new net::CmdWPacket(*dynamic_cast<net::CmdRPacket*>(o->packet));
		return 1;
	} else if(argtype == LUA_TTABLE) {
		net::CmdWPacket* wpk = new net::CmdWPacket(512);
		if(0 != luabin_pack_table(wpk,L,-1)){
			delete wpk;
			return luaL_error(L,"table should not hava metatable");	
		}else{
			lua_packet_t p = (lua_packet_t)lua_newuserdata(L, sizeof(*p));
			luaL_getmetatable(L, LUAWPACKET_METATABLE);
			lua_setmetatable(L, -2);
			p->packet = wpk;
		}
		return 1;
	} else {
		return luaL_error(L,"invaild opration for arg1");
	}
}

static int NewCmdRPacket(lua_State *L){
	if (lua_type(L,1) == LUA_TUSERDATA) {
		lua_packet_t o = lua_getluapacket(L,1);
		if(!o || !o->packet || o->packet->Type() != CMDRPACKET) {
			return luaL_error(L,"invaild opration for arg1");
		}
		lua_packet_t p = (lua_packet_t)lua_newuserdata(L, sizeof(*p));
		luaL_getmetatable(L, LUARPACKET_METATABLE);
		lua_setmetatable(L, -2);
		p->packet = new net::CmdRPacket(*dynamic_cast<net::CmdRPacket*>(o->packet));
		return 1;
	} else {
		return luaL_error(L,"invaild opration for arg1");
	}
}

static int destroy_luapacket(lua_State *L) {
	lua_packet_t p = lua_getluapacket(L,1);
	if(p->packet){
		delete p->packet;
	}
    return 0;
}

void push_luaPacket(lua_State *L,net::Packet *rpk){
	lua_packet_t p = (lua_packet_t)lua_newuserdata(L, sizeof(*p));
	luaL_getmetatable(L, LUARPACKET_METATABLE);
	lua_setmetatable(L, -2);
	p->packet = rpk->Clone();	
}

net::Packet *toLuaPacket(lua_State *L,int index){
	lua_packet_t p = lua_getluapacket(L,index);
	if(p) return p->packet;
	return NULL;
}

static int WriteCmd(lua_State *L){
	lua_packet_t p = lua_getluapacket(L,1);
	if (!p || p->packet)return luaL_error(L,"invaild opration");
	net::CmdWPacket *wpk = dynamic_cast<net::CmdWPacket*>(p->packet);	
	if(!wpk)return luaL_error(L,"invaild opration");
	if(lua_type(L,2) != LUA_TNUMBER)
		return luaL_error(L,"invaild arg2");
	wpk->WriteCmd((unsigned short)lua_tointeger(L,2));
	return 0;	
}

static int ReadCmd(lua_State *L){
	lua_packet_t p = lua_getluapacket(L,1);
	if (!p || p->packet) return luaL_error(L,"invaild opration");
	net::CmdRPacket *rpk = dynamic_cast<net::CmdRPacket*>(p->packet);
	if(!rpk) return luaL_error(L,"invaild opration");
	lua_pushinteger(L,rpk->ReadCmd());
	return 1;	
}

void RegLuaPacket(lua_State *L) {
	//RPacket
	luaL_newmetatable(L, LUARPACKET_METATABLE);
	lua_pushstring(L, "__gc");
	lua_pushcfunction(L, destroy_luapacket);
	lua_rawset(L, -3);

	lua_newtable(L);
	lua_pushstring(L, "ReadU8");
	lua_pushcfunction(L, ReadUint8);
	lua_rawset(L, -3);
	lua_pushstring(L, "ReadU16");
	lua_pushcfunction(L, ReadUint16);
	lua_rawset(L, -3);
	lua_pushstring(L, "ReadU32");
	lua_pushcfunction(L, ReadUint32);
	lua_rawset(L, -3);
	lua_pushstring(L, "ReadI32");
	lua_pushcfunction(L, ReadInt32);
	lua_rawset(L, -3);
	lua_pushstring(L, "ReadNum");
	lua_pushcfunction(L, ReadDouble);
	lua_rawset(L, -3);
	lua_pushstring(L, "ReadStr");
	lua_pushcfunction(L, ReadString);
	lua_rawset(L, -3);
	lua_pushstring(L, "ReadTable");
	lua_pushcfunction(L, ReadTable);
	lua_rawset(L, -3);

	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);



	//WPacket
	luaL_newmetatable(L, LUAWPACKET_METATABLE);
	lua_pushstring(L, "__gc");
	lua_pushcfunction(L, destroy_luapacket);
	lua_rawset(L, -3);

	lua_newtable(L);
	lua_pushstring(L, "WriteU8");
	lua_pushcfunction(L, WriteUint8);
	lua_rawset(L, -3);
	lua_pushstring(L, "WriteU16");
	lua_pushcfunction(L, WriteUint16);
	lua_rawset(L, -3);
	lua_pushstring(L, "WriteU32");
	lua_pushcfunction(L, WriteUint32);
	lua_rawset(L, -3);
	lua_pushstring(L, "WriteNum");
	lua_pushcfunction(L, WriteDouble);
	lua_rawset(L, -3);
	lua_pushstring(L, "WriteStr");
	lua_pushcfunction(L, WriteString);
	lua_rawset(L, -3);
	lua_pushstring(L, "WriteTable");
	lua_pushcfunction(L, WriteTable);
	lua_rawset(L, -3);
	lua_pushstring(L, "RewriteU8");
	lua_pushcfunction(L, RewriteUint8);
	lua_rawset(L, -3);
	lua_pushstring(L, "RewriteU16");
	lua_pushcfunction(L, RewriteUint16);
	lua_rawset(L, -3);
	lua_pushstring(L, "RewriteU32");
	lua_pushcfunction(L, RewriteUint32);
	lua_rawset(L, -3);
	lua_pushstring(L, "RewriteNum");
	lua_pushcfunction(L, RewriteDouble);
	lua_rawset(L, -3);
	lua_pushstring(L, "GetWritePos");
	lua_pushcfunction(L, GetWritePos);
	lua_rawset(L, -3);

    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);



	//CmdRPacket
	luaL_newmetatable(L, LUACMDRPACKET_METATABLE);
	lua_pushstring(L, "__gc");
	lua_pushcfunction(L, destroy_luapacket);
	lua_rawset(L, -3);

	lua_newtable(L);
	lua_pushstring(L, "Read_Cmd");
	lua_pushcfunction(L, ReadCmd);
	lua_rawset(L, -3);	
	lua_pushstring(L, "ReadU8");
	lua_pushcfunction(L, ReadUint8);
	lua_rawset(L, -3);
	lua_pushstring(L, "ReadU16");
	lua_pushcfunction(L, ReadUint16);
	lua_rawset(L, -3);
	lua_pushstring(L, "ReadU32");
	lua_pushcfunction(L, ReadUint32);
	lua_rawset(L, -3);
	lua_pushstring(L, "ReadI32");
	lua_pushcfunction(L, ReadInt32);
	lua_rawset(L, -3);
	lua_pushstring(L, "ReadNum");
	lua_pushcfunction(L, ReadDouble);
	lua_rawset(L, -3);
	lua_pushstring(L, "ReadStr");
	lua_pushcfunction(L, ReadString);
	lua_rawset(L, -3);
	lua_pushstring(L, "ReadTable");
	lua_pushcfunction(L, ReadTable);
	lua_rawset(L, -3);

	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);



	//CmdWPacket
	luaL_newmetatable(L, LUACMDWPACKET_METATABLE);
	lua_pushstring(L, "__gc");
	lua_pushcfunction(L, destroy_luapacket);
	lua_rawset(L, -3);

	lua_newtable(L);
	lua_newtable(L);
	lua_pushstring(L, "WriteCmd");
	lua_pushcfunction(L, WriteCmd);
	lua_rawset(L, -3);	
	lua_pushstring(L, "WriteU8");
	lua_pushcfunction(L, WriteUint8);
	lua_rawset(L, -3);
	lua_pushstring(L, "WriteU16");
	lua_pushcfunction(L, WriteUint16);
	lua_rawset(L, -3);
	lua_pushstring(L, "WriteU32");
	lua_pushcfunction(L, WriteUint32);
	lua_rawset(L, -3);
	lua_pushstring(L, "WriteNum");
	lua_pushcfunction(L, WriteDouble);
	lua_rawset(L, -3);
	lua_pushstring(L, "WriteStr");
	lua_pushcfunction(L, WriteString);
	lua_rawset(L, -3);
	lua_pushstring(L, "WriteTable");
	lua_pushcfunction(L, WriteTable);
	lua_rawset(L, -3);
	lua_pushstring(L, "RewriteU8");
	lua_pushcfunction(L, RewriteUint8);
	lua_rawset(L, -3);
	lua_pushstring(L, "RewriteU16");
	lua_pushcfunction(L, RewriteUint16);
	lua_rawset(L, -3);
	lua_pushstring(L, "RewriteU32");
	lua_pushcfunction(L, RewriteUint32);
	lua_rawset(L, -3);
	lua_pushstring(L, "RewriteNum");
	lua_pushcfunction(L, RewriteDouble);
	lua_rawset(L, -3);
	lua_pushstring(L, "GetWritePos");
	lua_pushcfunction(L, GetWritePos);
	lua_rawset(L, -3);

    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);    

	
	lua_pushstring(L, "NewWPacket");
	lua_pushcfunction(L, NewWPacket);
	lua_rawset(L, -3);
	lua_pushstring(L, "NewRPacket");
	lua_pushcfunction(L, NewRPacket);
	lua_rawset(L, -3);

	lua_pushstring(L, "NewCmdWPacket");
	lua_pushcfunction(L, NewCmdWPacket);
	lua_rawset(L, -3);
	lua_pushstring(L, "NewCmdRPacket");
	lua_pushcfunction(L, NewCmdRPacket);
	lua_rawset(L, -3);	

}