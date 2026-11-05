// CvLocalization (Localization::String, SharedString, ParameterArgument, StringDictionary), rewritten for the
// portable build. Partial: text keys are looked up in the Language_<type> table (Tag, Text) of the database
// given to Initialize/InitializeDictionary, and composition substitutes {N}, {N_Name} and {TXT_KEY_...}.
// Transforms after ':' : plural/gender choices ("plural 1?has; other?have;") are evaluated; number formats and :textkey insert the argument as is.
// The FireWorks headers expect the gamecore precompiled-header environment.
#include "CvGameCoreDLLPCH.h"

#include <stdio.h>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "CvGameDatabase.h"
#include "CvLocalization.h"
#include "lekmod_portable/host_log.h"

using namespace Localization;

namespace {

struct DictionaryState
{
	Database::Connection* db;
	std::string languageType;
	std::string table;
	std::auto_ptr<ILocalizationLogger> logger;
	DictionaryState() : db(0), languageType("en_US"), table("Language_en_US") {}
};

DictionaryState& state()
{
	static DictionaryState s;
	return s;
}

void array_delete(char* p) { delete[] p; }

bool lookup_text(const char* key, size_t len, std::string& out)
{
	DictionaryState& s = state();
	if (!s.db || !s.db->GetSQLite3() || !key) return false;
	char* sql = sqlite3_mprintf("SELECT Text FROM %q WHERE Tag = ?1 LIMIT 1", s.table.c_str());
	sqlite3_stmt* stmt = 0;
	bool found = false;
	if (sqlite3_prepare_v2(s.db->GetSQLite3(), sql, -1, &stmt, 0) == SQLITE_OK)
	{
		sqlite3_bind_text(stmt, 1, key, (int)len, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW)
		{
			const unsigned char* t = sqlite3_column_text(stmt, 0);
			out = t ? (const char*)t : "";
			found = true;
		}
	}
	sqlite3_finalize(stmt);
	sqlite3_free(sql);
	return found;
}

std::string shared_text(const SharedString& s)
{
	size_t n = 0;
	const char* p = s.GetString(n);
	return p ? std::string(p, n) : std::string();
}

std::string argument_text(const ParameterArgument& a)
{
	char buf[64];
	switch (a.m_type)
	{
	case ParameterArgument::ARGTYPE_INT:
		snprintf(buf, sizeof(buf), "%d", a.m_value.i);
		return buf;
	case ParameterArgument::ARGTYPE_INT64:
		snprintf(buf, sizeof(buf), "%lld", (long long)a.m_value.i64);
		return buf;
	case ParameterArgument::ARGTYPE_FLOAT:
		snprintf(buf, sizeof(buf), "%g", (double)a.m_value.f);
		return buf;
	case ParameterArgument::ARGTYPE_DOUBLE:
		snprintf(buf, sizeof(buf), "%g", a.m_value.d);
		return buf;
	default:
		if (a.m_str.get())
		{
			String copy(*a.m_str);
			return copy.toUTF8();
		}
		return std::string();
	}
}

// Text of a string argument or base string: a bare TXT_KEY_ resolves through the dictionary.
std::string resolve_key(const std::string& text)
{
	std::string out;
	if (text.compare(0, 8, "TXT_KEY_") == 0 && text.find(' ') == std::string::npos && lookup_text(text.c_str(), text.size(), out)) return out;
	return text;
}

// The count a plural choice uses: numbers by value (floats truncate), a string argument is singular (the dictionary rows carry
// no plurality here).
int plural_count(const ParameterArgument& a)
{
	switch (a.m_type)
	{
	case ParameterArgument::ARGTYPE_INT: return a.m_value.i;
	case ParameterArgument::ARGTYPE_INT64: return (int)a.m_value.i64;
	case ParameterArgument::ARGTYPE_FLOAT: return (int)a.m_value.f;
	case ParameterArgument::ARGTYPE_DOUBLE: return (int)a.m_value.d;
	default: return 1;
	}
}

// "1?has; other?have;" (the body of "{@1_CivName: plural 1?has; other?have;}"): the text of the entry whose condition is the
// count ("1", "2", ... or "one" for 1), else the "other" entry, else the first one. `gender` choices ("feminine?Lady; other?Lord;")
// have no gender to test here and take "other".
std::string choose_form(const std::string& body, bool byCount, int count)
{
	std::string first, other, hit;
	bool haveFirst = false, haveOther = false, haveHit = false;
	char num[32];
	snprintf(num, sizeof(num), "%d", count);
	size_t i = 0;
	while (i < body.size())
	{
		size_t end = body.find(';', i);
		if (end == std::string::npos) end = body.size();
		std::string entry = body.substr(i, end - i);
		i = end + 1;
		size_t q = entry.find('?');
		if (q == std::string::npos) continue;
		size_t b = 0;
		while (b < q && entry[b] == ' ') ++b;
		std::string cond = entry.substr(b, q - b);
		while (!cond.empty() && cond[cond.size() - 1] == ' ') cond.erase(cond.size() - 1);
		std::string text = entry.substr(q + 1);
		if (!haveFirst) { first = text; haveFirst = true; }
		if (cond == "other") { other = text; haveOther = true; }
		else if (byCount && !haveHit && (cond == num || (cond == "one" && count == 1))) { hit = text; haveHit = true; }
	}
	if (haveHit) return hit;
	if (haveOther) return other;
	return first;
}

std::string compose(const std::string& base, const std::vector<ParameterArgument>& args, int depth)
{
	std::string out;
	size_t i = 0;
	while (i < base.size())
	{
		char c = base[i];
		if (c != '{')
		{
			out += c;
			++i;
			continue;
		}
		size_t close = base.find('}', i);
		if (close == std::string::npos)
		{
			out.append(base, i, std::string::npos);
			break;
		}
		std::string tag = base.substr(i + 1, close - i - 1);
		size_t colon = tag.find(':');
		std::string head = colon == std::string::npos ? tag : tag.substr(0, colon);
		while (!head.empty() && head[head.size() - 1] == ' ') head.erase(head.size() - 1);
		// "{1}", "{1_Name}" and "{@1_Name}" (a bare argument slot with an annotation; "{@1: plural ...}" keeps its colon and stays literal).
		const bool atSlot = head.size() > 1 && head[0] == '@' && head[1] >= '1' && head[1] <= '9';
		if (atSlot || (!head.empty() && head[0] >= '1' && head[0] <= '9'))
		{
			int index = atoi(head.c_str() + (atSlot ? 1 : 0));
			const bool have = index >= 1 && (size_t)index <= args.size();
			// "{1_Num: plural 1?Turn; other?Turns;}" and "{@1_CivName: plural 1?has; other?have;}" print the chosen form, not the
			// argument; "{@1: gender feminine?Lady; other?Lord;}" the neutral one. Other transforms (":textkey", ":number ...")
			// insert the argument as is.
			std::string xf = colon == std::string::npos ? std::string() : tag.substr(colon + 1);
			size_t xs = 0;
			while (xs < xf.size() && xf[xs] == ' ') ++xs;
			size_t xe = xs;
			while (xe < xf.size() && xf[xe] != ' ') ++xe;
			const std::string xname = xf.substr(xs, xe - xs);
			if (xname == "plural") out += choose_form(xf.substr(xe), true, have ? plural_count(args[(size_t)index - 1]) : 1);
			else if (xname == "gender") out += choose_form(xf.substr(xe), false, 0);
			else if (have) out += argument_text(args[(size_t)index - 1]);
		}
		else if (head.compare(0, 8, "TXT_KEY_") == 0 && depth < 8)
		{
			std::string text;
			if (lookup_text(head.c_str(), head.size(), text)) out += compose(text, args, depth + 1);
			else out += head;
		}
		else
		{
			out.append(base, i, close - i + 1);
		}
		i = close + 1;
	}
	return out;
}

}  // namespace

// ---- StringParameter / ParameterArgument ---------------------------------------------------------
StringParameter::StringParameter()
	: ArgumentIndex(0), uiTextKeyStart(0), lenTextKey(0), uiParameterStart(0), uiParameterEnd(0), uiTransformTagStart(0), uiTransformTagEnd(0)
{
}
StringParameter::StringParameter(const StringParameter& o)
	: ArgumentIndex(o.ArgumentIndex), uiTextKeyStart(o.uiTextKeyStart), lenTextKey(o.lenTextKey), uiParameterStart(o.uiParameterStart),
	  uiParameterEnd(o.uiParameterEnd), uiTransformTagStart(o.uiTransformTagStart), uiTransformTagEnd(o.uiTransformTagEnd),
	  m_injected_parameters(o.m_injected_parameters)
{
}

ParameterArgument::ParameterArgument(const ParameterArgument& o) : m_value(o.m_value), m_str(o.m_str), m_type(o.m_type) {}
ParameterArgument::ParameterArgument(const String& strText) : m_str(new String(strText)), m_type(ARGTYPE_STRING) { m_value.i64 = 0; }
ParameterArgument::ParameterArgument(const int val) : m_type(ARGTYPE_INT)
{
	m_value.i64 = 0;
	m_value.i = val;
}
ParameterArgument::ParameterArgument(const __int64 val) : m_type(ARGTYPE_INT64) { m_value.i64 = val; }
ParameterArgument::ParameterArgument(const float val) : m_type(ARGTYPE_FLOAT)
{
	m_value.i64 = 0;
	m_value.f = val;
}
ParameterArgument::ParameterArgument(const double val) : m_type(ARGTYPE_DOUBLE) { m_value.d = val; }

// ---- SharedString --------------------------------------------------------------------------------
SharedString::SharedString() : m_BufferLength(0), m_offset(0), m_length(0) {}
SharedString::SharedString(const char* szString, size_t length) : m_BufferLength(length), m_offset(0), m_length(length)
{
	char* p = new char[length + 1];
	if (length && szString) memcpy(p, szString, length);
	p[length] = 0;
	m_Buffer.reset(p, array_delete);
}
SharedString::SharedString(const SharedString& o) : m_Buffer(o.m_Buffer), m_BufferLength(o.m_BufferLength), m_offset(o.m_offset), m_length(o.m_length) {}
SharedString& SharedString::operator=(const SharedString& o)
{
	m_Buffer = o.m_Buffer;
	m_BufferLength = o.m_BufferLength;
	m_offset = o.m_offset;
	m_length = o.m_length;
	return *this;
}
const char* SharedString::GetBuffer(size_t& length) const
{
	length = m_BufferLength;
	return m_Buffer.get() ? m_Buffer.get() : "";
}
const char* SharedString::GetString(size_t& length) const
{
	length = m_length;
	return m_Buffer.get() ? m_Buffer.get() + m_offset : "";
}
size_t SharedString::GetBufferLength() const { return m_BufferLength; }
size_t SharedString::GetOffset() const { return m_offset; }
size_t SharedString::GetLength() const { return m_length; }
bool SharedString::SetOffsetAndLength(size_t offset, size_t length)
{
	if (offset + length > m_BufferLength) return false;
	m_offset = offset;
	m_length = length;
	return true;
}

// ---- SingleFormString (forms are not split in this implementation) ---------------------------------
SingleFormString::SingleFormString() : m_Plurality(0), m_Gender(GENDER_NEUTER), m_Composed(false), m_Parsed(false) {}
SingleFormString::SingleFormString(const SharedString& baseString, size_t plurality, Gender gender)
	: m_BaseString(baseString), m_Plurality(plurality), m_Gender(gender), m_Composed(false), m_Parsed(false)
{
}
SingleFormString::~SingleFormString() {}
SingleFormString& SingleFormString::operator=(const SingleFormString& o)
{
	m_BaseString = o.m_BaseString;
	m_ComposedString = o.m_ComposedString;
	m_Plurality = o.m_Plurality;
	m_Gender = o.m_Gender;
	m_Traits = o.m_Traits;
	m_Parameters = o.m_Parameters;
	m_Composed = o.m_Composed;
	m_Parsed = o.m_Parsed;
	return *this;
}
const SharedString& SingleFormString::GetBaseString() const { return m_BaseString; }
const SharedString& SingleFormString::GetComposedString() const { return m_Composed ? m_ComposedString : m_BaseString; }
void SingleFormString::SetPlurality() {}
size_t SingleFormString::GetPlurality() const { return m_Plurality; }
void SingleFormString::SetGender(Gender gender) { m_Gender = gender; }
Gender SingleFormString::GetGender() const { return (Gender)m_Gender; }
bool SingleFormString::HasTrait(const char* szTrait, size_t lenTrait) const
{
	for (size_t i = 0; i < m_Traits.size(); ++i)
		if (m_Traits[i].size() == lenTrait && m_Traits[i].compare(0, lenTrait, szTrait, lenTrait) == 0) return true;
	return false;
}
bool SingleFormString::Parse()
{
	m_Parsed = true;
	return true;
}
bool SingleFormString::Compose(const std::vector<ParameterArgument>& arguments)
{
	std::string s = compose(shared_text(m_BaseString), arguments, 0);
	m_ComposedString = SharedString(s.c_str(), s.size());
	m_Composed = true;
	return true;
}

// ---- String --------------------------------------------------------------------------------------
String String::Empty;

String::String() : m_pkLanguage(0) {}
String::String(const char* szString) : m_pkLanguage(0)
{
	size_t n = szString ? strlen(szString) : 0;
	m_BaseString = SharedString(szString ? szString : "", n);
}
String::String(const char* szString, unsigned int uiLen) : m_BaseString(szString ? szString : "", szString ? uiLen : 0), m_pkLanguage(0) {}
String::String(const std::string& str) : m_BaseString(str.c_str(), strlen(str.c_str())), m_pkLanguage(0) {}
String::String(const String& o)
	: m_BaseString(o.m_BaseString), m_ComposedNullTerminated(o.m_ComposedNullTerminated), m_arguments(o.m_arguments), m_Forms(o.m_Forms), m_pkLanguage(o.m_pkLanguage)
{
}
String::~String() {}

bool String::IsEmpty() const { return m_BaseString.GetLength() == 0; }

String& String::operator=(const String& o)
{
	if (this != &o)
	{
		m_BaseString = o.m_BaseString;
		m_ComposedNullTerminated = o.m_ComposedNullTerminated;
		m_arguments = o.m_arguments;
		m_Forms = o.m_Forms;
		m_pkLanguage = o.m_pkLanguage;
	}
	return *this;
}

String& String::operator=(const char* szBaseString)
{
	size_t n = szBaseString ? strlen(szBaseString) : 0;
	m_BaseString = SharedString(szBaseString ? szBaseString : "", n);
	m_ComposedNullTerminated = SharedString();
	m_arguments.clear();
	m_Forms.clear();
	return *this;
}

const char* String::toUTF8()
{
	size_t bytes = 0;
	return toUTF8(bytes, 1);
}

namespace {
// A composed string whose buffer is never empty, so String::toUTF8 can tell "composed" from "not yet composed" even for "".
SharedString composed_string(const std::string& s)
{
	SharedString ss(s.c_str(), s.size() + 1);
	ss.SetOffsetAndLength(0, s.size());
	return ss;
}
}  // namespace

const char* String::toUTF8(size_t& bytes, int)
{
	// Compose once: the callers take several pointers from one String (CvTeam.cpp:6375 passes toUTF8() twice), and a recompose would free the first buffer.
	if (m_ComposedNullTerminated.GetBufferLength() == 0) Compose(1);
	return m_ComposedNullTerminated.GetString(bytes);
}

const char* String::GetBaseString(size_t& bytes) const { return m_BaseString.GetString(bytes); }
Language* String::GetLanguage() const { return m_pkLanguage; }
bool String::SetLanguage(Language* language)
{
	m_pkLanguage = language;
	return true;
}
size_t String::GetArgumentCount() const { return m_arguments.size(); }

bool String::PushArgument(const char* szText) { return PushArgument(String(szText)); }
bool String::PushArgument(const char* szText, size_t length) { return PushArgument(String(szText, (unsigned int)length)); }
bool String::PushArgument(const int val) { return PushArgument(ParameterArgument(val)); }
bool String::PushArgument(const __int64 val) { return PushArgument(ParameterArgument(val)); }
bool String::PushArgument(const float val) { return PushArgument(ParameterArgument(val)); }
bool String::PushArgument(const double val) { return PushArgument(ParameterArgument(val)); }
bool String::PushArgument(const String& strText) { return PushArgument(ParameterArgument(strText)); }
bool String::PushArgument(const ParameterArgument& argument)
{
	m_arguments.push_back(argument);
	m_ComposedNullTerminated = SharedString();
	return true;
}

size_t String::GetNumForms() const { return 1; }
size_t String::GetFormByPlurality(int, int) const { return 1; }
size_t String::GetFormByGender(const char*, size_t, int) const { return 1; }
size_t String::GetFormByPluralityAndGender(int, const char*, size_t, int) const { return 1; }
bool String::HasForm(int nForm) const { return nForm == 1; }
bool String::HasFormByPlurality(int, int nOffset) const { return nOffset == 0; }
bool String::HasFormByGender(const char*, size_t, int nOffset) const { return nOffset == 0; }
bool String::HasFormByPluralityAndGender(int, const char*, size_t, int nOffset) const { return nOffset == 0; }
bool String::SetGender(const char*, size_t) { return true; }
bool String::SetGender(size_t, const char*, size_t) { return true; }
bool String::SetGender(Gender) { return true; }
bool String::SetGender(size_t, Gender) { return true; }
Gender String::GetGender(size_t) const { return GENDER_NEUTER; }
bool String::SetPlurality(const char*, size_t) { return true; }
bool String::SetPlurality(size_t) { return true; }
bool String::SetPlurality(size_t, size_t) { return true; }
size_t String::GetPlurality(size_t) const { return 1; }

bool String::Compose(size_t)
{
	std::string base = resolve_key(shared_text(m_BaseString));
	std::string s = compose(base, m_arguments, 0);
	m_ComposedNullTerminated = composed_string(s);
	return true;
}

bool String::Parse(size_t) { return true; }

std::ostream& operator<<(std::ostream& out, Localization::String& s) { return out << s.toUTF8(); }

// ---- Language (one language, the dictionary's current one) ----------------------------------------
Language::Language(const char*, IPluralityRule*) {}
Language::Language(const Language&) {}
Language::~Language() {}
const char* Language::GetType() const { return state().languageType.c_str(); }
unsigned int Language::GetPluralForm(int nElements) const { return nElements == 1 ? 0u : 1u; }
IPluralityRule* Language::GetPluralityRule() const { return 0; }
void Language::SetPluralityRule(IPluralityRule*) {}
bool Language::Compare(const char* a, size_t la, const char* b, size_t lb, int& result) const
{
	int c = memcmp(a, b, la < lb ? la : lb);
	result = c != 0 ? c : (la < lb ? -1 : (la > lb ? 1 : 0));
	return true;
}
const ParameterTransforms& Language::GetParameterTransforms() const
{
	static ParameterTransforms s_transforms("en_US");
	return s_transforms;
}

// Locale transforms are not implemented: every Format/Choice call reports failure (the caller keeps the raw value).
ParameterTransforms::ParameterTransforms(const char*) {}
ParameterTransforms::~ParameterTransforms() {}
#define LEKMOD_NO_TRANSFORM(NAME) \
	bool ParameterTransforms::NAME(const ParameterArgument&, const std::string&, std::string&) const { return false; }
LEKMOD_NO_TRANSFORM(FormatCurrency)
LEKMOD_NO_TRANSFORM(FormatNumber)
LEKMOD_NO_TRANSFORM(FormatPercent)
LEKMOD_NO_TRANSFORM(FormatRoman)
LEKMOD_NO_TRANSFORM(FormatSpellout)
LEKMOD_NO_TRANSFORM(FormatUppercase)
LEKMOD_NO_TRANSFORM(FormatLowercase)
LEKMOD_NO_TRANSFORM(FormatTitlecase)
LEKMOD_NO_TRANSFORM(FormatTextkey)
LEKMOD_NO_TRANSFORM(ChoiceFormatGender)
LEKMOD_NO_TRANSFORM(ChoiceFormatPlurality)
LEKMOD_NO_TRANSFORM(ChoiceFormatText)
#undef LEKMOD_NO_TRANSFORM

// ---- StringDictionary and the module functions -----------------------------------------------------
namespace {
StringDictionary g_dictionary;
}

const char* StringDictionary::Language::GetType() const { return state().languageType.c_str(); }
const char* StringDictionary::Language::GetDisplayName() const { return state().languageType.c_str(); }
int StringDictionary::Language::GetPluralityRule() const { return 1; }

String StringDictionary::Lookup(const char* szTextKey) const { return Lookup(szTextKey, szTextKey ? (unsigned int)strlen(szTextKey) : 0); }
String StringDictionary::Lookup(const char* szTextKey, unsigned int lenTextKey) const
{
	String out;
	if (TryLookup(szTextKey, lenTextKey, out)) return out;
	return String(szTextKey, lenTextKey);  // the key itself, like the original for a missing tag
}
bool StringDictionary::TryLookup(const char* szTextKey, unsigned int lenTextKey, String& output) const
{
	std::string text;
	if (!lookup_text(szTextKey, lenTextKey, text)) return false;
	output = String(text.c_str(), (unsigned int)text.size());
	return true;
}
String StringDictionary::LookupLanguage(const char* szTextKey, unsigned int) const { return Lookup(szTextKey); }
String StringDictionary::LookupLanguage(const char* szTextKey, unsigned int lenTextKey, unsigned int) const { return Lookup(szTextKey, lenTextKey); }
bool StringDictionary::TryLookupLanguage(const char* szTextKey, unsigned int lenTextKey, unsigned int, String& output) const
{
	return TryLookup(szTextKey, lenTextKey, output);
}
bool StringDictionary::HasKey(const char* szTextKey, unsigned int lenTextKey) const
{
	std::string text;
	return lookup_text(szTextKey, lenTextKey, text);
}
bool StringDictionary::HasKey(const char* szTextKey, unsigned int lenTextKey, unsigned int) const { return HasKey(szTextKey, lenTextKey); }
unsigned int StringDictionary::GetCurrentLanguageIndex() const { return 0; }
bool StringDictionary::SetCurrentLanguage(unsigned int) { return true; }
bool StringDictionary::SetCurrentLanguage(const char* szLanguageType) { return Localization::SetCurrentLanguage(szLanguageType); }
size_t StringDictionary::GetLanguageCount() const { return 1; }
const char* StringDictionary::GetLanguageType(unsigned int) const { return state().languageType.c_str(); }
const char* StringDictionary::GetLanguageDisplayName(unsigned int) const { return state().languageType.c_str(); }
const StringDictionary::Language& StringDictionary::GetLanguage(unsigned int) const
{
	static Language s_language;
	return s_language;
}
void StringDictionary::NotifyDatabaseUpdated() const {}

namespace Localization {

bool Initialize(const wchar_t*, Database::Connection& db, const char* szLanguagesTableName, const char* szLanguageType, bool bDisableFallback)
{
	return InitializeDictionary(db, szLanguagesTableName, szLanguageType, bDisableFallback);
}

bool Shutdown() { return ShutdownDictionary(); }

bool SetCurrentLanguage(const char* szType)
{
	if (!szType || !*szType) return false;
	state().languageType = szType;
	state().table = std::string("Language_") + szType;
	return true;
}

const Language& GetCurrentLanguage()
{
	static Language s_language("en_US", 0);
	return s_language;
}

const StringDictionary& GetStringDictionary() { return g_dictionary; }
String Lookup(const char* textKey) { return g_dictionary.Lookup(textKey); }

bool ShutdownDictionary()
{
	state().db = 0;
	return true;
}

bool InitializeDictionary(Database::Connection& db, const char*, const char* szLanguageType, bool)
{
	state().db = &db;
	SetCurrentLanguage(szLanguageType ? szLanguageType : "en_US");
	return true;
}

void SetLogger(ILocalizationLogger* pkLogger) { state().logger.reset(pkLogger); }
ILocalizationLogger* GetLogger() { return state().logger.get(); }
void LogMessage(const char* m)
{
	if (state().logger.get()) state().logger->Log(m, ILocalizationLogger::TYPE_MESSAGE);
	else lekmod_portable::Log(lekmod_portable::LOG_DEBUG, "localization: %s", m);
}
void LogWarning(const char* m)
{
	if (state().logger.get()) state().logger->Log(m, ILocalizationLogger::TYPE_WARNING);
	else lekmod_portable::Log(lekmod_portable::LOG_WARN, "localization: %s", m);
}
void LogError(const char* m)
{
	if (state().logger.get()) state().logger->Log(m, ILocalizationLogger::TYPE_ERROR);
	else lekmod_portable::Log(lekmod_portable::LOG_ERROR, "localization: %s", m);
}

static String format_time(const SYSTEMTIME& t, bool date, bool time)
{
	char buf[64];
	if (date && time) snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
	else if (date) snprintf(buf, sizeof(buf), "%04u-%02u-%02u", t.wYear, t.wMonth, t.wDay);
	else snprintf(buf, sizeof(buf), "%02u:%02u", t.wHour, t.wMinute);
	return String(buf);
}
String ConvertUTCDatetoLocalTZDate(SYSTEMTIME& st, const char*) { return format_time(st, true, false); }
String ConvertUTCTimetoLocalTZTime(SYSTEMTIME& st, const char*) { return format_time(st, false, true); }
String ConvertUTCDateTimetoLocalTZDateTime(SYSTEMTIME& st, const char*, const char*) { return format_time(st, true, true); }

}  // namespace Localization
