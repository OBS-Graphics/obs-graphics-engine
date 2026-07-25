// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "lua_xml.h"

#include <sol/sol.hpp>

#include <pugixml.hpp>

#include <string>

namespace lua_xml {
namespace {

// ── pugixml -> Lua ────────────────────────────────────────────────────────

sol::table NodeToLua(sol::state_view lua, const pugi::xml_node& node, int depth, std::string& err);

// Direct text/CDATA children only, concatenated. Text nested inside a child
// element belongs to that child, not here.
std::string DirectText(const pugi::xml_node& node)
{
    std::string text;
    for (pugi::xml_node child : node.children()) {
        if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata)
            text += child.value();
    }
    return text;
}

sol::table NodeToLua(sol::state_view lua, const pugi::xml_node& node, int depth, std::string& err)
{
    sol::table t = lua.create_table();
    if (depth > kMaxDepth) {
        err = "XML nesting deeper than " + std::to_string(kMaxDepth) + " levels";
        return t;
    }

    t["name"] = std::string(node.name());
    t["text"] = DirectText(node);

    sol::table attrs = lua.create_table();
    for (pugi::xml_attribute a : node.attributes())
        attrs[std::string(a.name())] = std::string(a.value());
    t["attr"] = attrs;

    sol::table children = lua.create_table();
    int i = 1;
    for (pugi::xml_node child : node.children()) {
        // Comments and processing instructions carry no data a script wants;
        // text/CDATA already went into `text` above.
        if (child.type() != pugi::node_element) continue;
        children[i++] = NodeToLua(lua, child, depth + 1, err);
        if (!err.empty()) break;
    }
    t["children"] = children;

    return t;
}

// ── bindings ──────────────────────────────────────────────────────────────

sol::variadic_results Fail(sol::state_view lua, const std::string& message)
{
    sol::variadic_results r;
    r.push_back(sol::make_object(lua, sol::lua_nil));
    r.push_back(sol::make_object(lua, message));
    return r;
}

sol::variadic_results Decode(const std::string& text, sol::this_state ts)
{
    sol::state_view lua(ts);

    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_buffer(text.data(), text.size());
    if (!result) return Fail(lua, result.description());

    // A well-formed document has exactly one element child; that's the root
    // the script cares about, not the document node wrapping it.
    pugi::xml_node root = doc.document_element();
    if (!root) return Fail(lua, "document has no root element");

    std::string err;
    sol::table node = NodeToLua(lua, root, 0, err);
    if (!err.empty()) return Fail(lua, err);

    sol::variadic_results r;
    r.push_back(node);
    return r;
}

// Both find helpers scan `node.children` rather than the node itself, so
// passing a node that has no `children` field is a plain miss, not an error.
sol::optional<sol::table> Children(const sol::table& node)
{
    return node["children"];
}

sol::object Find(const sol::table& node, const std::string& name, sol::this_state ts)
{
    sol::state_view lua(ts);
    auto children = Children(node);
    if (!children) return sol::make_object(lua, sol::lua_nil);

    for (const auto& [k, v] : *children) {
        (void)k;
        if (v.get_type() != sol::type::table) continue;
        sol::table child = v.as<sol::table>();
        sol::optional<std::string> childName = child["name"];
        if (childName && *childName == name) return sol::make_object(lua, child);
    }
    return sol::make_object(lua, sol::lua_nil);
}

sol::table FindAll(const sol::table& node, const std::string& name, sol::this_state ts)
{
    sol::state_view lua(ts);
    sol::table out = lua.create_table();

    auto children = Children(node);
    if (!children) return out;

    int i = 1;
    for (const auto& [k, v] : *children) {
        (void)k;
        if (v.get_type() != sol::type::table) continue;
        sol::table child = v.as<sol::table>();
        sol::optional<std::string> childName = child["name"];
        if (childName && *childName == name) out[i++] = child;
    }
    return out;
}

}  // namespace

void Register(sol::state& L)
{
    sol::table xml = L.create_named_table("xml");
    xml.set_function("decode", &Decode);
    xml.set_function("find", &Find);
    xml.set_function("find_all", &FindAll);
}

}  // namespace lua_xml
