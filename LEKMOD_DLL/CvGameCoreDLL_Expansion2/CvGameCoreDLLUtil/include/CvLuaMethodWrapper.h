
#pragma once 

#include "CvLuaArgTemplates.h"

template< class Derived, class InstanceType>
class CvLuaMethodWrapper
{

protected:
	//These are helper templates that will allow for quick and easy member function wrapping when
	//implementing a Lua method.
	//They currently do not support non-native types or optional values so the scope is quite limited.
	//regular variations (const)
	template<typename ret>
	static int BasicLuaMethod(lua_State* L, ret (InstanceType::*func)() const);
	template<typename ret, typename arg1>
	static int BasicLuaMethod(lua_State* L, ret (InstanceType::*func)(arg1) const);
	template<typename ret, typename arg1, typename arg2>
	static int BasicLuaMethod(lua_State* L, ret (InstanceType::*func)(arg1, arg2) const);
	template<typename ret, typename arg1, typename arg2, typename arg3>
	static int BasicLuaMethod(lua_State* L, ret (InstanceType::*func)(arg1, arg2, arg3) const);
	template<typename ret, typename arg1, typename arg2, typename arg3, typename arg4>
	static int BasicLuaMethod(lua_State* L, ret (InstanceType::*func)(arg1, arg2, arg3, arg4) const);

	//regular variations (non const)
	template<typename ret>
	static int BasicLuaMethod(lua_State* L, ret (InstanceType::*func)());
	template<typename ret, typename arg1>
	static int BasicLuaMethod(lua_State* L, ret (InstanceType::*func)(arg1));
	template<typename ret, typename arg1, typename arg2>
	static int BasicLuaMethod(lua_State* L, ret (InstanceType::*func)(arg1, arg2));
	template<typename ret, typename arg1, typename arg2, typename arg3>
	static int BasicLuaMethod(lua_State* L, ret (InstanceType::*func)(arg1, arg2, arg3));
	template<typename ret, typename arg1, typename arg2, typename arg3, typename arg4>
	static int BasicLuaMethod(lua_State* L, ret (InstanceType::*func)(arg1, arg2, arg3, arg4));

	//void variations (const)
	static int BasicLuaMethod(lua_State* L, void (InstanceType::*func)() const);
	template<typename arg1>
	static int BasicLuaMethod(lua_State* L, void (InstanceType::*func)(arg1) const);
	template<typename arg1, typename arg2>
	static int BasicLuaMethod(lua_State* L, void (InstanceType::*func)(arg1, arg2) const);
	template<typename arg1, typename arg2, typename arg3>
	static int BasicLuaMethod(lua_State* L, void (InstanceType::*func)(arg1, arg2, arg3) const);
	template<typename arg1, typename arg2, typename arg3, typename arg4>
	static int BasicLuaMethod(lua_State* L, void (InstanceType::*func)(arg1, arg2, arg3, arg4) const);

	//void variations (non const)
	static int BasicLuaMethod(lua_State* L, void (InstanceType::*func)());
	template<typename arg1>
	static int BasicLuaMethod(lua_State* L, void (InstanceType::*func)(arg1));
	template<typename arg1, typename arg2>
	static int BasicLuaMethod(lua_State* L, void (InstanceType::*func)(arg1, arg2));
	template<typename arg1, typename arg2, typename arg3>
	static int BasicLuaMethod(lua_State* L, void (InstanceType::*func)(arg1, arg2, arg3));
	template<typename arg1, typename arg2, typename arg3, typename arg4>
	static int BasicLuaMethod(lua_State* L, void (InstanceType::*func)(arg1, arg2, arg3, arg4));
#if defined(LEKMOD_PORTABLE)
	// MSVC deduces these templates from members inherited from a base of InstanceType (e.g. &CvPlayerAI::f
	// naming a CvPlayer member); standard deduction does not convert. Forward through an explicit cast.
	template<typename Owner, typename ret>
	static int BasicLuaMethod(lua_State* L, ret (Owner::*func)())
	{
		return BasicLuaMethod(L, static_cast<ret (InstanceType::*)()>(func));
	}
	template<typename Owner, typename ret>
	static int BasicLuaMethod(lua_State* L, ret (Owner::*func)() const)
	{
		return BasicLuaMethod(L, static_cast<ret (InstanceType::*)() const>(func));
	}
	template<typename Owner, typename ret, typename a1>
	static int BasicLuaMethod(lua_State* L, ret (Owner::*func)(a1))
	{
		return BasicLuaMethod(L, static_cast<ret (InstanceType::*)(a1)>(func));
	}
	template<typename Owner, typename ret, typename a1>
	static int BasicLuaMethod(lua_State* L, ret (Owner::*func)(a1) const)
	{
		return BasicLuaMethod(L, static_cast<ret (InstanceType::*)(a1) const>(func));
	}
	template<typename Owner, typename ret, typename a1, typename a2>
	static int BasicLuaMethod(lua_State* L, ret (Owner::*func)(a1, a2))
	{
		return BasicLuaMethod(L, static_cast<ret (InstanceType::*)(a1, a2)>(func));
	}
	template<typename Owner, typename ret, typename a1, typename a2>
	static int BasicLuaMethod(lua_State* L, ret (Owner::*func)(a1, a2) const)
	{
		return BasicLuaMethod(L, static_cast<ret (InstanceType::*)(a1, a2) const>(func));
	}
	template<typename Owner, typename ret, typename a1, typename a2, typename a3>
	static int BasicLuaMethod(lua_State* L, ret (Owner::*func)(a1, a2, a3))
	{
		return BasicLuaMethod(L, static_cast<ret (InstanceType::*)(a1, a2, a3)>(func));
	}
	template<typename Owner, typename ret, typename a1, typename a2, typename a3>
	static int BasicLuaMethod(lua_State* L, ret (Owner::*func)(a1, a2, a3) const)
	{
		return BasicLuaMethod(L, static_cast<ret (InstanceType::*)(a1, a2, a3) const>(func));
	}
	template<typename Owner, typename ret, typename a1, typename a2, typename a3, typename a4>
	static int BasicLuaMethod(lua_State* L, ret (Owner::*func)(a1, a2, a3, a4))
	{
		return BasicLuaMethod(L, static_cast<ret (InstanceType::*)(a1, a2, a3, a4)>(func));
	}
	template<typename Owner, typename ret, typename a1, typename a2, typename a3, typename a4>
	static int BasicLuaMethod(lua_State* L, ret (Owner::*func)(a1, a2, a3, a4) const)
	{
		return BasicLuaMethod(L, static_cast<ret (InstanceType::*)(a1, a2, a3, a4) const>(func));
	}
#endif
};

//------------------------------------------------------------------------------
// template members
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
// regular variations (const)
//------------------------------------------------------------------------------
template<class Derived, class InstanceType> template<typename ret>
int CvLuaMethodWrapper<Derived, InstanceType>::BasicLuaMethod(lua_State* L, ret (InstanceType::*func)() const)
{
	InstanceType* pkType = Derived::GetInstance(L);
	CvLuaArgs::pushValue<ret>(L, (pkType->*func)());

	return 1;
}
//------------------------------------------------------------------------------
template<class Derived, class InstanceType> template<typename ret, typename arg1>
int CvLuaMethodWrapper<Derived, InstanceType>::BasicLuaMethod(lua_State* L, ret (InstanceType::*func)(arg1) const)
{
	const int idx = Derived::GetStartingArgIndex();
	InstanceType* pkType = Derived::GetInstance(L);
	CvLuaArgs::pushValue<ret>(L, (pkType->*func)(CvLuaArgs::toValue<arg1>(L, idx)));

	return 1;
}
//------------------------------------------------------------------------------
template<class Derived, class InstanceType> template<typename ret, typename arg1, typename arg2>
int CvLuaMethodWrapper<Derived, InstanceType>::BasicLuaMethod(lua_State* L, ret (InstanceType::*func)(arg1, arg2) const)
{
	const int idx = Derived::GetStartingArgIndex();
	InstanceType* pkType = Derived::GetInstance(L);
	CvLuaArgs::pushValue<ret>(L, (pkType->*func)(CvLuaArgs::toValue<arg1>(L, idx), CvLuaArgs::toValue<arg2>(L, idx + 1)));

	return 1;
}
//------------------------------------------------------------------------------
template<class Derived, class InstanceType> template<typename ret, typename arg1, typename arg2, typename arg3>
int CvLuaMethodWrapper<Derived, InstanceType>::BasicLuaMethod(lua_State* L, ret (InstanceType::*func)(arg1, arg2, arg3) const)
{
	const int idx = Derived::GetStartingArgIndex();
	InstanceType* pkType = Derived::GetInstance(L);
	CvLuaArgs::pushValue<ret>(L, (pkType->*func)(CvLuaArgs::toValue<arg1>(L, idx), CvLuaArgs::toValue<arg2>(L, idx + 1), CvLuaArgs::toValue<arg3>(L, idx + 2)));

	return 1;
}
//------------------------------------------------------------------------------
template<class Derived, class InstanceType> template<typename ret, typename arg1, typename arg2, typename arg3, typename arg4>
int CvLuaMethodWrapper<Derived, InstanceType>::BasicLuaMethod(lua_State* L, ret (InstanceType::*func)(arg1, arg2, arg3, arg4) const)
{
	const int idx = Derived::GetStartingArgIndex();
	InstanceType* pkType = Derived::GetInstance(L);
	CvLuaArgs::pushValue<ret>(L, (pkType->*func)(CvLuaArgs::toValue<arg1>(L, idx), CvLuaArgs::toValue<arg2>(L, idx + 1), CvLuaArgs::toValue<arg3>(L, idx + 2), CvLuaArgs::toValue<arg4>(L, idx + 3)));

	return 1;
}

//------------------------------------------------------------------------------
// regular variations (non const)
//------------------------------------------------------------------------------
template<class Derived, class InstanceType> template<typename ret>
int CvLuaMethodWrapper<Derived, InstanceType>::BasicLuaMethod(lua_State* L, ret (InstanceType::*func)())
{
	InstanceType* pkType = Derived::GetInstance(L);
	CvLuaArgs::pushValue<ret>(L, (pkType->*func)());

	return 1;
}
//------------------------------------------------------------------------------
template<class Derived, class InstanceType> template<typename ret, typename arg1>
int CvLuaMethodWrapper<Derived, InstanceType>::BasicLuaMethod(lua_State* L, ret (InstanceType::*func)(arg1))
{
	const int idx = Derived::GetStartingArgIndex();
	InstanceType* pkType = Derived::GetInstance(L);
	CvLuaArgs::pushValue<ret>(L, (pkType->*func)(CvLuaArgs::toValue<arg1>(L, idx)));

	return 1;
}
//------------------------------------------------------------------------------
template<class Derived, class InstanceType> template<typename ret, typename arg1, typename arg2>
int CvLuaMethodWrapper<Derived, InstanceType>::BasicLuaMethod(lua_State* L, ret (InstanceType::*func)(arg1, arg2))
{
	const int idx = Derived::GetStartingArgIndex();
	InstanceType* pkType = Derived::GetInstance(L);
	CvLuaArgs::pushValue<ret>(L, (pkType->*func)(CvLuaArgs::toValue<arg1>(L, idx), CvLuaArgs::toValue<arg2>(L, idx + 1)));

	return 1;
}
//------------------------------------------------------------------------------
template<class Derived, class InstanceType> template<typename ret, typename arg1, typename arg2, typename arg3>
int CvLuaMethodWrapper<Derived, InstanceType>::BasicLuaMethod(lua_State* L, ret (InstanceType::*func)(arg1, arg2, arg3))
{
	const int idx = Derived::GetStartingArgIndex();
	InstanceType* pkType = Derived::GetInstance(L);
	CvLuaArgs::pushValue<ret>(L, (pkType->*func)(CvLuaArgs::toValue<arg1>(L, idx), CvLuaArgs::toValue<arg2>(L, idx + 1), CvLuaArgs::toValue<arg3>(L, idx + 2)));

	return 1;
}
//------------------------------------------------------------------------------
template<class Derived, class InstanceType> template<typename ret, typename arg1, typename arg2, typename arg3, typename arg4>
int CvLuaMethodWrapper<Derived, InstanceType>::BasicLuaMethod(lua_State* L, ret (InstanceType::*func)(arg1, arg2, arg3, arg4))
{
	const int idx = Derived::GetStartingArgIndex();
	InstanceType* pkType = Derived::GetInstance(L);
	CvLuaArgs::pushValue<ret>(L, (pkType->*func)(CvLuaArgs::toValue<arg1>(L, idx), CvLuaArgs::toValue<arg2>(L, idx + 1), CvLuaArgs::toValue<arg3>(L, idx + 2), CvLuaArgs::toValue<arg4>(L, idx + 3)));

	return 1;
}

//------------------------------------------------------------------------------
// void variations (const)
//------------------------------------------------------------------------------
template<class Derived, class InstanceType>
int CvLuaMethodWrapper<Derived, InstanceType>::BasicLuaMethod(lua_State* L, void (InstanceType::*func)() const)
{
	InstanceType* pkType = Derived::GetInstance(L);
	(pkType->*func)();
	return 0;
}
//------------------------------------------------------------------------------
template<class Derived, class InstanceType> template<typename arg1>
int CvLuaMethodWrapper<Derived, InstanceType>::BasicLuaMethod(lua_State* L, void (InstanceType::*func)(arg1) const)
{
	const int idx = Derived::GetStartingArgIndex();
	InstanceType* pkType = Derived::GetInstance(L);
	(pkType->*func)(CvLuaArgs::toValue<arg1>(L, idx));
	return 0;
}
//------------------------------------------------------------------------------
template<class Derived, class InstanceType> template<typename arg1, typename arg2>
int CvLuaMethodWrapper<Derived, InstanceType>::BasicLuaMethod(lua_State* L, void (InstanceType::*func)(arg1, arg2) const)
{
	const int idx = Derived::GetStartingArgIndex();
	InstanceType* pkType = Derived::GetInstance(L);
	(pkType->*func)(CvLuaArgs::toValue<arg1>(L, idx), CvLuaArgs::toValue<arg2>(L, idx + 1));
	return 0;
}

//------------------------------------------------------------------------------
template<class Derived, class InstanceType> template<typename arg1, typename arg2, typename arg3>
int CvLuaMethodWrapper<Derived, InstanceType>::BasicLuaMethod(lua_State* L, void (InstanceType::*func)(arg1, arg2, arg3) const)
{
	const int idx = Derived::GetStartingArgIndex();
	InstanceType* pkType = Derived::GetInstance(L);
	(pkType->*func)(CvLuaArgs::toValue<arg1>(L, idx), CvLuaArgs::toValue<arg2>(L, idx + 1), CvLuaArgs::toValue<arg3>(L, idx + 2));
	return 0;
}

//------------------------------------------------------------------------------
template<class Derived, class InstanceType> template<typename arg1, typename arg2, typename arg3, typename arg4>
int CvLuaMethodWrapper<Derived, InstanceType>::BasicLuaMethod(lua_State* L, void (InstanceType::*func)(arg1, arg2, arg3, arg4) const)
{
	const int idx = Derived::GetStartingArgIndex();
	InstanceType* pkType = Derived::GetInstance(L);
	(pkType->*func)(CvLuaArgs::toValue<arg1>(L, idx), CvLuaArgs::toValue<arg2>(L, idx + 1), CvLuaArgs::toValue<arg3>(L, idx + 2), CvLuaArgs::toValue<arg4>(L, idx + 3));
	return 0;
}

//------------------------------------------------------------------------------
// void variations (non const)
//------------------------------------------------------------------------------
template<class Derived, class InstanceType>
int CvLuaMethodWrapper<Derived, InstanceType>::BasicLuaMethod(lua_State* L, void (InstanceType::*func)())
{
	InstanceType* pkType = Derived::GetInstance(L);
	(pkType->*func)();
	return 0;
}
//------------------------------------------------------------------------------
template<class Derived, class InstanceType> template<typename arg1>
int CvLuaMethodWrapper<Derived, InstanceType>::BasicLuaMethod(lua_State* L, void (InstanceType::*func)(arg1))
{
	const int idx = Derived::GetStartingArgIndex();
	InstanceType* pkType = Derived::GetInstance(L);
	(pkType->*func)(CvLuaArgs::toValue<arg1>(L, idx));
	return 0;
}
//------------------------------------------------------------------------------
template<class Derived, class InstanceType> template<typename arg1, typename arg2>
int CvLuaMethodWrapper<Derived, InstanceType>::BasicLuaMethod(lua_State* L, void (InstanceType::*func)(arg1, arg2))
{
	const int idx = Derived::GetStartingArgIndex();
	InstanceType* pkType = Derived::GetInstance(L);
	(pkType->*func)(CvLuaArgs::toValue<arg1>(L, idx), CvLuaArgs::toValue<arg2>(L, idx + 1));
	return 0;
}

//------------------------------------------------------------------------------
template<class Derived, class InstanceType> template<typename arg1, typename arg2, typename arg3>
int CvLuaMethodWrapper<Derived, InstanceType>::BasicLuaMethod(lua_State* L, void (InstanceType::*func)(arg1, arg2, arg3))
{
	const int idx = Derived::GetStartingArgIndex();
	InstanceType* pkType = Derived::GetInstance(L);
	(pkType->*func)(CvLuaArgs::toValue<arg1>(L, idx), CvLuaArgs::toValue<arg2>(L, idx + 1), CvLuaArgs::toValue<arg3>(L, idx + 2));
	return 0;
}

//------------------------------------------------------------------------------
template<class Derived, class InstanceType> template<typename arg1, typename arg2, typename arg3, typename arg4>
int CvLuaMethodWrapper<Derived, InstanceType>::BasicLuaMethod(lua_State* L, void (InstanceType::*func)(arg1, arg2, arg3, arg4))
{
	const int idx = Derived::GetStartingArgIndex();
	InstanceType* pkType = Derived::GetInstance(L);
	(pkType->*func)(CvLuaArgs::toValue<arg1>(L, idx), CvLuaArgs::toValue<arg2>(L, idx + 1), CvLuaArgs::toValue<arg3>(L, idx + 2), CvLuaArgs::toValue<arg4>(L, idx + 3));
	return 0;
}