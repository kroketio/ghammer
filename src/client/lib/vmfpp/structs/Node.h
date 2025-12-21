#pragma once

#include <QObject>
#include <QDebug>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vmfpp {

// Transparent hashing so lookups by string_view do not have to build a std::string.
struct StringKeyHash {
  using is_transparent = void;

  std::size_t operator()(std::string_view key) const noexcept {
    return std::hash<std::string_view>{}(key);
  }
};

// A KeyValue object with one key, multiple keyvalues
class Node {
public:
    using ValueMap = std::unordered_map<std::string, std::vector<std::string>, StringKeyHash, std::equal_to<>>;
    using ChildMap = std::unordered_map<std::string, std::vector<Node>, StringKeyHash, std::equal_to<>>;

    Node() = default;

    [[nodiscard]] const ValueMap& getValues() const;

    [[nodiscard]] ValueMap& getValues();

    [[nodiscard]] bool hasValue(std::string_view key) const;

    [[nodiscard]] const std::vector<std::string>& getValue(std::string_view key) const;

    [[nodiscard]] std::vector<std::string>& getValue(std::string_view key);

    // Non-throwing lookup: nullptr when the key is absent.
    [[nodiscard]] const std::vector<std::string>* findValue(std::string_view key) const;

    [[nodiscard]] std::vector<std::string>* findValueMut(std::string_view key);

    void addValue(std::string key, std::string value);

    void removeValue(std::string_view key);

    [[nodiscard]] const ChildMap& getChildren() const;

    [[nodiscard]] ChildMap& getChildren();

    [[nodiscard]] bool hasChild(std::string_view key) const;

    [[nodiscard]] const std::vector<Node>& getChild(std::string_view key) const;

    [[nodiscard]] std::vector<Node>& getChild(std::string_view key);

    // Non-throwing lookup: nullptr when the key is absent.
    [[nodiscard]] const std::vector<Node>* findChild(std::string_view key) const;

    [[nodiscard]] std::vector<Node>* findChildMut(std::string_view key);

    void addChild(std::string key, Node&& value);

    void removeChild(std::string_view key);

    [[nodiscard]] std::size_t getNumItems() const;

protected:
    ValueMap values;
    ChildMap children;
};

} // namespace vmfpp
