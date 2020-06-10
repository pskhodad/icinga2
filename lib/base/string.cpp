/* Icinga 2 | (c) 2012 Icinga GmbH | GPLv2+ */

#include "base/string.hpp"
#include "base/value.hpp"
#include "base/primitivetype.hpp"
#include "base/dictionary.hpp"
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/algorithm/string/split.hpp>
#include <ostream>

using namespace icinga;

template class std::vector<String>;

REGISTER_BUILTIN_TYPE(String, String::GetPrototype());

const String::SizeType String::NPos = std::string::npos;

static const Shared<std::string>::Ptr& GetDefaultString()
{
	static const auto defaultString (Shared<std::string>::Make());
	return defaultString;
}

String::String()
	: m_Data(GetDefaultString())
{ }

String::String(const char *data)
	: m_Data(Shared<std::string>::Make(data))
{ }

String::String(std::string data)
	: m_Data(Shared<std::string>::Make(std::move(data)))
{ }

String::String(String::SizeType n, char c)
	: m_Data(Shared<std::string>::Make(n, c))
{ }

String::String(String&& other)
	: String((const String&)other)
{ }

#ifndef _MSC_VER
String::String(Value&& other)
{
	*this = std::move(other);
}
#endif /* _MSC_VER */

String& String::operator=(Value&& other)
{
	if (other.IsString())
		*this = std::move(other.Get<String>());
	else
		*this = static_cast<String>(other);

	return *this;
}

String& String::operator+=(const Value& rhs)
{
	*this += static_cast<String>(rhs);
	return *this;
}

String& String::operator=(String&& rhs)
{
	*this = (const String&)rhs;
	return *this;
}

String& String::operator=(const std::string& rhs)
{
	*this = String(rhs);
	return *this;
}

String& String::operator=(const char *rhs)
{
	*this = String(rhs);
	return *this;
}

const char& String::operator[](String::SizeType pos) const
{
	return (*m_Data)[pos];
}

char& String::operator[](String::SizeType pos)
{
	CoW();
	return (*m_Data)[pos];
}

String& String::operator+=(const String& rhs)
{
	CoW();
	*m_Data += *rhs.m_Data;
	return *this;
}

String& String::operator+=(const char *rhs)
{
	CoW();
	*m_Data += rhs;
	return *this;
}

String& String::operator+=(char rhs)
{
	CoW();
	*m_Data += rhs;
	return *this;
}

bool String::IsEmpty() const
{
	return m_Data->empty();
}

bool String::operator<(const String& rhs) const
{
	return *m_Data < *rhs.m_Data;
}

String::operator const std::string&() const
{
	return *m_Data;
}

const char *String::CStr() const
{
	return m_Data->c_str();
}

void String::Clear()
{
	CoW();
	m_Data->clear();
}

String::SizeType String::GetLength() const
{
	return m_Data->size();
}

std::string& String::GetData()
{
	CoW();
	return *m_Data;
}

const std::string& String::GetData() const
{
	return *m_Data;
}

String::SizeType String::Find(const String& str, String::SizeType pos) const
{
	return m_Data->find(*str.m_Data, pos);
}

String::SizeType String::RFind(const String& str, String::SizeType pos) const
{
	return m_Data->rfind(*str.m_Data, pos);
}

String::SizeType String::FindFirstOf(const char *s, String::SizeType pos) const
{
	return m_Data->find_first_of(s, pos);
}

String::SizeType String::FindFirstOf(char ch, String::SizeType pos) const
{
	return m_Data->find_first_of(ch, pos);
}

String::SizeType String::FindFirstNotOf(const char *s, String::SizeType pos) const
{
	return m_Data->find_first_not_of(s, pos);
}

String::SizeType String::FindFirstNotOf(char ch, String::SizeType pos) const
{
	return m_Data->find_first_not_of(ch, pos);
}

String::SizeType String::FindLastOf(const char *s, String::SizeType pos) const
{
	return m_Data->find_last_of(s, pos);
}

String::SizeType String::FindLastOf(char ch, String::SizeType pos) const
{
	return m_Data->find_last_of(ch, pos);
}

String String::SubStr(String::SizeType first, String::SizeType len) const
{
	return m_Data->substr(first, len);
}

std::vector<String> String::Split(const char *separators) const
{
	std::vector<String> result;
	boost::algorithm::split(result, *m_Data, boost::is_any_of(separators));
	return result;
}

void String::Replace(String::SizeType first, String::SizeType second, const String& str)
{
	CoW();
	m_Data->replace(first, second, str);
}

String String::Trim() const
{
	std::string t = *m_Data;
	boost::algorithm::trim(t);
	return std::move(t);
}

String String::ToLower() const
{
	std::string t = *m_Data;
	boost::algorithm::to_lower(t);
	return std::move(t);
}

String String::ToUpper() const
{
	std::string t = *m_Data;
	boost::algorithm::to_upper(t);
	return std::move(t);
}

String String::Reverse() const
{
	std::string t = *m_Data;
	std::reverse(t.begin(), t.end());
	return std::move(t);
}

void String::Append(int count, char ch)
{
	CoW();
	m_Data->append(count, ch);
}

bool String::Contains(const String& str) const
{
	return (m_Data->find(*str.m_Data) != std::string::npos);
}

void String::swap(String& str)
{
	m_Data.swap(str.m_Data);
}

String::Iterator String::erase(String::Iterator first, String::Iterator last)
{
	CoW();
	return m_Data->erase(first, last);
}

String::Iterator String::Begin()
{
	CoW();
	return m_Data->begin();
}

String::ConstIterator String::Begin() const
{
	return m_Data->begin();
}

String::Iterator String::End()
{
	CoW();
	return m_Data->end();
}

String::ConstIterator String::End() const
{
	return m_Data->end();
}

String::ReverseIterator String::RBegin()
{
	CoW();
	return m_Data->rbegin();
}

String::ConstReverseIterator String::RBegin() const
{
	return m_Data->rbegin();
}

String::ReverseIterator String::REnd()
{
	CoW();
	return m_Data->rend();
}

String::ConstReverseIterator String::REnd() const
{
	return m_Data->rend();
}

std::ostream& icinga::operator<<(std::ostream& stream, const String& str)
{
	stream << str.GetData();
	return stream;
}

std::istream& icinga::operator>>(std::istream& stream, String& str)
{
	std::string tstr;
	stream >> tstr;
	str = tstr;
	return stream;
}

String icinga::operator+(const String& lhs, const String& rhs)
{
	return lhs.GetData() + rhs.GetData();
}

String icinga::operator+(const String& lhs, const char *rhs)
{
	return lhs.GetData() + rhs;
}

String icinga::operator+(const char *lhs, const String& rhs)
{
	return lhs + rhs.GetData();
}

bool icinga::operator==(const String& lhs, const String& rhs)
{
	return lhs.GetData() == rhs.GetData();
}

bool icinga::operator==(const String& lhs, const char *rhs)
{
	return lhs.GetData() == rhs;
}

bool icinga::operator==(const char *lhs, const String& rhs)
{
	return lhs == rhs.GetData();
}

bool icinga::operator<(const String& lhs, const char *rhs)
{
	return lhs.GetData() < rhs;
}

bool icinga::operator<(const char *lhs, const String& rhs)
{
	return lhs < rhs.GetData();
}

bool icinga::operator>(const String& lhs, const String& rhs)
{
	return lhs.GetData() > rhs.GetData();
}

bool icinga::operator>(const String& lhs, const char *rhs)
{
	return lhs.GetData() > rhs;
}

bool icinga::operator>(const char *lhs, const String& rhs)
{
	return lhs > rhs.GetData();
}

bool icinga::operator<=(const String& lhs, const String& rhs)
{
	return lhs.GetData() <= rhs.GetData();
}

bool icinga::operator<=(const String& lhs, const char *rhs)
{
	return lhs.GetData() <= rhs;
}

bool icinga::operator<=(const char *lhs, const String& rhs)
{
	return lhs <= rhs.GetData();
}

bool icinga::operator>=(const String& lhs, const String& rhs)
{
	return lhs.GetData() >= rhs.GetData();
}

bool icinga::operator>=(const String& lhs, const char *rhs)
{
	return lhs.GetData() >= rhs;
}

bool icinga::operator>=(const char *lhs, const String& rhs)
{
	return lhs >= rhs.GetData();
}

bool icinga::operator!=(const String& lhs, const String& rhs)
{
	return lhs.GetData() != rhs.GetData();
}

bool icinga::operator!=(const String& lhs, const char *rhs)
{
	return lhs.GetData() != rhs;
}

bool icinga::operator!=(const char *lhs, const String& rhs)
{
	return lhs != rhs.GetData();
}

String::Iterator icinga::begin(String& x)
{
	return x.Begin();
}

String::ConstIterator icinga::begin(const String& x)
{
	return x.Begin();
}

String::Iterator icinga::end(String& x)
{
	return x.End();
}

String::ConstIterator icinga::end(const String& x)
{
	return x.End();
}
String::Iterator icinga::range_begin(String& x)
{
	return x.Begin();
}

String::ConstIterator icinga::range_begin(const String& x)
{
	return x.Begin();
}

String::Iterator icinga::range_end(String& x)
{
	return x.End();
}

String::ConstIterator icinga::range_end(const String& x)
{
	return x.End();
}

void String::CoW()
{
	if (m_Data->GetReferences() > 1u) {
		m_Data = Shared<std::string>::Make(*m_Data);
	}
}
