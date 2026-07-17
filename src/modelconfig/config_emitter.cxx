/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include "config_emitter.h"

#include "configstoreadapter.h"
#include <codes/configfile.h>

#include <vector>

namespace codes {
namespace config {

namespace {

void emit_keys(ConfigVTable* cf, SectionHandle sec, const std::vector<compiled_key>& keys) {
    for (const compiled_key& k : keys) {
        std::vector<const char*> vals;
        vals.reserve(k.values.size());
        for (const std::string& v : k.values)
            vals.push_back(v.c_str());
        cf_createKey(cf, sec, k.name.c_str(), vals.data(), (unsigned int)vals.size());
    }
}

void emit_section(ConfigVTable* cf, SectionHandle parent, const compiled_section& s) {
    SectionHandle sec;
    cf_createSection(cf, parent, s.name.c_str(), &sec);
    emit_keys(cf, sec, s.keys);
    for (const compiled_section& sub : s.subsections)
        emit_section(cf, sec, sub);
}

} // namespace

ConfigVTable* emit(const compiled_config& cfg) {
    ConfigVTable* cf = cfsa_create_empty();
    for (const compiled_section& s : cfg.sections)
        emit_section(cf, ROOT_SECTION, s);
    return cf;
}

} // namespace config
} // namespace codes
