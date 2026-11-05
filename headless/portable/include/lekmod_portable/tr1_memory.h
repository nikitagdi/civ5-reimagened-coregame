// std::tr1::shared_ptr for C++03 (MSVC 2008 TR1; libc++ has no tr1 namespace). Single-threaded refcount.
#ifndef LEKMOD_PORTABLE_TR1_MEMORY_H
#define LEKMOD_PORTABLE_TR1_MEMORY_H

#include <algorithm>
#include <cstddef>

namespace lekmod_portable {

struct sp_counted_base
{
	long uses;
	sp_counted_base() : uses(1) {}
	virtual ~sp_counted_base() {}
	virtual void dispose() = 0;
};

template <class P, class D>
struct sp_counted_impl : sp_counted_base
{
	P ptr;
	D del;
	sp_counted_impl(P p, D d) : ptr(p), del(d) {}
	virtual void dispose() { del(ptr); }
};

template <class T>
struct sp_default_delete
{
	void operator()(T* p) const { delete p; }
};

}  // namespace lekmod_portable

namespace std {
namespace tr1 {

template <class T>
class shared_ptr
{
public:
	typedef T element_type;

	shared_ptr() : m_ptr(0), m_count(0) {}
	template <class Y>
	explicit shared_ptr(Y* p) : m_ptr(p), m_count(0)
	{
		if (p) m_count = new lekmod_portable::sp_counted_impl<Y*, lekmod_portable::sp_default_delete<Y> >(p, lekmod_portable::sp_default_delete<Y>());
	}
	template <class Y, class D>
	shared_ptr(Y* p, D d) : m_ptr(p), m_count(new lekmod_portable::sp_counted_impl<Y*, D>(p, d))
	{
	}
	shared_ptr(const shared_ptr& o) : m_ptr(o.m_ptr), m_count(o.m_count)
	{
		if (m_count) ++m_count->uses;
	}
	template <class Y>
	shared_ptr(const shared_ptr<Y>& o) : m_ptr(o.m_ptr), m_count(o.m_count)
	{
		if (m_count) ++m_count->uses;
	}
	~shared_ptr() { release(); }

	shared_ptr& operator=(const shared_ptr& o)
	{
		shared_ptr(o).swap(*this);
		return *this;
	}
	template <class Y>
	shared_ptr& operator=(const shared_ptr<Y>& o)
	{
		shared_ptr(o).swap(*this);
		return *this;
	}

	void reset() { shared_ptr().swap(*this); }
	template <class Y>
	void reset(Y* p) { shared_ptr(p).swap(*this); }
	template <class Y, class D>
	void reset(Y* p, D d) { shared_ptr(p, d).swap(*this); }

	T* get() const { return m_ptr; }
	T& operator*() const { return *m_ptr; }
	T* operator->() const { return m_ptr; }
	long use_count() const { return m_count ? m_count->uses : 0; }
	bool unique() const { return use_count() == 1; }
	typedef T* shared_ptr::*unspecified_bool_type;
	operator unspecified_bool_type() const { return m_ptr ? &shared_ptr::m_ptr : 0; }
	void swap(shared_ptr& o)
	{
		std::swap(m_ptr, o.m_ptr);
		std::swap(m_count, o.m_count);
	}

private:
	template <class Y>
	friend class shared_ptr;
	void release()
	{
		if (m_count && --m_count->uses == 0)
		{
			m_count->dispose();
			delete m_count;
		}
		m_ptr = 0;
		m_count = 0;
	}
	T* m_ptr;
	lekmod_portable::sp_counted_base* m_count;
};

template <class T, class U>
bool operator==(const shared_ptr<T>& a, const shared_ptr<U>& b) { return a.get() == b.get(); }
template <class T, class U>
bool operator!=(const shared_ptr<T>& a, const shared_ptr<U>& b) { return a.get() != b.get(); }
template <class T, class U>
bool operator<(const shared_ptr<T>& a, const shared_ptr<U>& b) { return a.get() < b.get(); }

}  // namespace tr1
}  // namespace std

#endif  // LEKMOD_PORTABLE_TR1_MEMORY_H
