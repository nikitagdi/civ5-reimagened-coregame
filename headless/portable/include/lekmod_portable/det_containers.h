// Deterministic stand-ins for stdext::hash_map/hash_set and std::tr1::unordered_map/unordered_set.
// They are ordered trees: iteration order is the key order on every target (libstdc++ and libc++ hash
// differently, which would make Linux and WASM iterate, and serialize, in different orders).
// Iteration order therefore differs from the MSVC 2008 build, where it followed the hash buckets.
#ifndef LEKMOD_PORTABLE_DET_CONTAINERS_H
#define LEKMOD_PORTABLE_DET_CONTAINERS_H

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

namespace lekmod_portable {

// Adapts an MSVC hash_compare-style traits class (operator()(a, b) is "less") to a std::map comparator.
template <class Traits>
struct traits_less
{
	Traits traits;
	template <class K>
	bool operator()(const K& a, const K& b) const { return traits(a, b); }
};

// FNV-1a over the bytes of the value; only used where code asks for a hash value explicitly.
template <class K>
struct det_hash
{
	size_t operator()(const K& k) const
	{
		const unsigned char* p = reinterpret_cast<const unsigned char*>(&k);
		unsigned int h = 2166136261u;
		for (size_t i = 0; i < sizeof(K); ++i) h = (h ^ p[i]) * 16777619u;
		return h;
	}
};
template <class C, class T, class A>
struct det_hash<std::basic_string<C, T, A> >
{
	size_t operator()(const std::basic_string<C, T, A>& s) const
	{
		unsigned int h = 2166136261u;
		for (size_t i = 0; i < s.size(); ++i) h = (h ^ (unsigned int)s[i]) * 16777619u;
		return h;
	}
};

}  // namespace lekmod_portable

namespace stdext {

template <class K, class Pr = std::less<K> >
class hash_compare
{
public:
	enum
	{
		bucket_size = 4,
		min_buckets = 8
	};
	hash_compare() {}
	explicit hash_compare(Pr pr) : comp(pr) {}
	size_t operator()(const K& k) const { return lekmod_portable::det_hash<K>()(k); }
	bool operator()(const K& a, const K& b) const { return comp(a, b); }

protected:
	Pr comp;
};

template <class K, class V, class Tr = hash_compare<K, std::less<K> >, class A = std::allocator<std::pair<const K, V> > >
class hash_map : public std::map<K, V, lekmod_portable::traits_less<Tr>, A>
{
	typedef std::map<K, V, lekmod_portable::traits_less<Tr>, A> base;

public:
	hash_map() {}
	template <class It>
	hash_map(It first, It last) : base(first, last) {}
};

template <class K, class V, class Tr = hash_compare<K, std::less<K> >, class A = std::allocator<std::pair<const K, V> > >
class hash_multimap : public std::multimap<K, V, lekmod_portable::traits_less<Tr>, A>
{
};

template <class K, class Tr = hash_compare<K, std::less<K> >, class A = std::allocator<K> >
class hash_set : public std::set<K, lekmod_portable::traits_less<Tr>, A>
{
	typedef std::set<K, lekmod_portable::traits_less<Tr>, A> base;

public:
	hash_set() {}
	template <class It>
	hash_set(It first, It last) : base(first, last) {}
};

}  // namespace stdext

namespace std {
namespace tr1 {

template <class K>
struct hash : lekmod_portable::det_hash<K>
{
};

template <class K, class V, class H = hash<K>, class E = std::equal_to<K>, class A = std::allocator<std::pair<const K, V> > >
class unordered_map : public std::map<K, V, std::less<K>, A>
{
	typedef std::map<K, V, std::less<K>, A> base;

public:
	typedef H hasher;
	typedef E key_equal;
	unordered_map() {}
	explicit unordered_map(size_t) {}
	template <class It>
	unordered_map(It first, It last) : base(first, last) {}
	void rehash(size_t) {}
	void reserve(size_t) {}
};

template <class K, class V, class H = hash<K>, class E = std::equal_to<K>, class A = std::allocator<std::pair<const K, V> > >
class unordered_multimap : public std::multimap<K, V, std::less<K>, A>
{
};

template <class K, class H = hash<K>, class E = std::equal_to<K>, class A = std::allocator<K> >
class unordered_set : public std::set<K, std::less<K>, A>
{
	typedef std::set<K, std::less<K>, A> base;

public:
	typedef H hasher;
	typedef E key_equal;
	unordered_set() {}
	explicit unordered_set(size_t) {}
	template <class It>
	unordered_set(It first, It last) : base(first, last) {}
	void rehash(size_t) {}
	void reserve(size_t) {}
};

}  // namespace tr1
}  // namespace std

#endif  // LEKMOD_PORTABLE_DET_CONTAINERS_H
