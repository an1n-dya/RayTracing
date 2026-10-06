#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// Minimal JSON document model, parser and pretty-printer - just enough for scene files.
// Only SceneSerializer depends on it, so it can be swapped for a full library (e.g. nlohmann/json) later.
namespace Json {

	class Value {
	public:
		enum class Type { Null, Bool, Number, String, Array, Object };

		Value() = default;
		Value(bool value) : m_Type(Type::Bool), m_Bool(value) {}
		Value(double value) : m_Type(Type::Number), m_Number(value) {}
		Value(float value) : m_Type(Type::Number), m_Number(value) {}
		Value(int value) : m_Type(Type::Number), m_Number(value) {}
		Value(const char* value) : m_Type(Type::String), m_String(value) {}
		Value(std::string value) : m_Type(Type::String), m_String(std::move(value)) {}

		static Value MakeArray() { Value value; value.m_Type = Type::Array; return value; }
		static Value MakeObject() { Value value; value.m_Type = Type::Object; return value; }

		Type GetType() const { return m_Type; }
		bool IsNull() const { return m_Type == Type::Null; }
		bool IsBool() const { return m_Type == Type::Bool; }
		bool IsNumber() const { return m_Type == Type::Number; }
		bool IsString() const { return m_Type == Type::String; }
		bool IsArray() const { return m_Type == Type::Array; }
		bool IsObject() const { return m_Type == Type::Object; }

		// Typed reads return the fallback when the value has a different type
		bool AsBool(bool fallback = false) const { return IsBool() ? m_Bool : fallback; }
		double AsNumber(double fallback = 0.0) const { return IsNumber() ? m_Number : fallback; }
		float AsFloat(float fallback = 0.0f) const { return IsNumber() ? (float)m_Number : fallback; }
		int AsInt(int fallback = 0) const { return IsNumber() ? (int)m_Number : fallback; }
		std::string AsString(const std::string& fallback = {}) const { return IsString() ? m_String : fallback; }

		// Arrays
		size_t Size() const { return IsArray() ? m_Array.size() : IsObject() ? m_Object.size() : 0; }
		const Value& operator[](size_t index) const;
		Value& Append(Value value);
		const std::vector<Value>& Elements() const { return m_Array; }

		// Objects (members keep their insertion order, so written files stay readable)
		const Value* Find(const std::string& key) const;
		const Value& operator[](const std::string& key) const; // null value if missing
		Value& operator[](const std::string& key);             // inserts if missing (turns null into an object)
		const std::vector<std::pair<std::string, Value>>& Members() const { return m_Object; }

	private:
		Type m_Type = Type::Null;
		bool m_Bool = false;
		double m_Number = 0.0;
		std::string m_String;
		std::vector<Value> m_Array;
		std::vector<std::pair<std::string, Value>> m_Object;
	};

	// Returns false and describes the problem (with line:column) in error on malformed input
	bool Parse(const std::string& text, Value& out, std::string* error = nullptr);

	// Pretty-printed with the given indent; arrays of plain numbers/bools stay on one line
	std::string Write(const Value& value, int indent = 2);

	bool ReadFile(const std::string& path, Value& out, std::string* error = nullptr);
	bool WriteFile(const std::string& path, const Value& value, std::string* error = nullptr);

}
