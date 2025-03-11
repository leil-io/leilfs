/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include "cfg.h"

#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <yaml-cpp/yaml.h>

#include "common/massert.h"

static char *cfgfname;
static std::map<std::string, std::string> configParameters;
static int logundefined=0;

int cfg_reload(void) {
	int lu = logundefined;
	std::string configfname(cfgfname);
	cfg_term();
	return cfg_load(configfname.c_str(), lu);
}

static int cfg_do_load(void) {
	constexpr size_t MAX_LINE_LEN = 1000;
	char linebuff[MAX_LINE_LEN];
	uint32_t nps, npe, vps, vpe, i;
	FILE *fd = fopen(cfgfname, "r");

	if (!fd) {
		safs::log_err("can't load config file: {}", cfgfname);
		return 1;
	}

	while (fgets(linebuff, MAX_LINE_LEN - 1, fd)) {
		linebuff[MAX_LINE_LEN - 1] = 0;

		if (linebuff[0] == '#')
			continue;

		for (i = 0; isspace(linebuff[i]); i++);

		for (nps = i; isupper(linebuff[i]) || linebuff[i] == '_'; i++);

		for (npe = i; isspace(linebuff[i]); i++);

		if (linebuff[i] != '=' || npe <= nps) {
			if (linebuff[i] > ' ')
				safs::log_warn("bad definition in config file '{}': {}", cfgfname, linebuff);
			continue;
		}

		for (i++; isspace(linebuff[i]); i++);

		for (vps = i; linebuff[i] >= ' ' && linebuff[i] < 127; i++);

		for (; i > vps && linebuff[i - 1] == ' '; i--);

		for (vpe = i; isspace(linebuff[i]); i++);

		if ((linebuff[i] != '\0' && linebuff[i] != '\r' &&
		     linebuff[i] != '\n' && linebuff[i] != '#') || vps == vpe) {
			safs::log_warn("bad definition in config file '{}': {}", cfgfname, linebuff);
			continue;
		}
		linebuff[npe] = 0;
		linebuff[vpe] = 0;

		try {
			std::string key(linebuff + nps, linebuff + npe);
			std::string value(linebuff + vps, linebuff + vpe);
			configParameters[key] = value;
		} catch (const std::exception& e) {
			safs::log_err("could not set config file {} values in key map: {}", cfgfname, e.what());
			fclose(fd);
			cfg_term();
			return -1;
		}
	}
	fclose(fd);
	return 0;
}

int cfg_load(const char *configfname,int _lu) {
	logundefined = _lu;
	cfgfname = strdup(configfname);

	return cfg_do_load();
}

std::string cfg_filename() {
	return cfgfname ? cfgfname : "";
}

int cfg_isdefined(const char *name) {
	return configParameters.count(name);
}

void cfg_term(void) {
	configParameters.clear();
	free(cfgfname);
}

// Helper function to setup an initial YAML map of maps for a particular
// service_name. After calling this function, config should be ended with
// YAML::EndMap once done before returning it as a string.
void start_yaml_emitter(std::string &service_name, YAML::Emitter &config) {
	config << YAML::BeginMap;
	config << YAML::Key << service_name;
	config << YAML::Value;
}

// Helper function to finish up a YAML configuration as a string.
std::string end_yaml_emitter(YAML::Emitter &config) {
	config << YAML::EndMap;
	assert(config.good());
	return config.c_str();
}

// Modifies an already existing YAML::Emitter to include the raw configuration
// values. You might want to setup some extra data, for example the service name
// of the configuration before calling this. The parameters are by default the
// configParameters, but you may provide your own map to use.
// You should set the loadValues to true if the values of the map are YAML
// key-value strings already.
// N.B! Configuration should be initialized before calling this function if you
// don't provide your own map.
void cfg_emitter_add_parameters(
    YAML::Emitter &config,
    const std::map<std::string, std::string> &parameters = configParameters,
    bool yamlValues = false) {
	config << YAML::BeginMap;
	for (const auto &[key, val] : parameters) {
		config << YAML::Key << key;
		yamlValues ? (config << YAML::Value << YAML::Load(val))
		           : (config << YAML::Value << val);
	}
	config << YAML::EndMap;
}

std::string cfg_yaml_string(std::string service_name) {
	YAML::Emitter config;
	start_yaml_emitter(service_name, config);
	cfg_emitter_add_parameters(config);
	return end_yaml_emitter(config);
}

std::string cfg_yaml_string() {
	YAML::Emitter config;
	cfg_emitter_add_parameters(config);
	assert(config.good());
	return config.c_str();
}

std::string cfg_yaml_list(std::string service_name,
                          std::map<std::string, std::string> &services) {
	YAML::Emitter config;
	start_yaml_emitter(service_name, config);
	cfg_emitter_add_parameters(config, services, true);
	return end_yaml_emitter(config);
}

std::string trimSpaces(const std::string& str)
{
    auto start = str.begin();
    while (start != str.end() && (std::isspace(*start) != 0)) {
        start++;
    }

    auto end = str.rbegin();
    while (end != str.rend() && (std::isspace(*end) != 0)) {
        end++;
    }

    return std::string(start, end.base());
}

int64_t cfg_parse_size(const std::string &str) {
	static const std::unordered_map<std::string, double> units{
	    {"b", 1.0},
	    // base 10
	    {"k", 1e3},
	    {"kb", 1e3},
	    {"m", 1e6},
	    {"mb", 1e6},
	    {"g", 1e9},
	    {"gb", 1e9},
	    {"t", 1e12},
	    {"tb", 1e12},
	    {"p", 1e15},
	    {"pb", 1e15},
	    {"e", 1e18},
	    {"eb", 1e18},
	    // base 2
	    {"ki", 1024.0},
	    {"kib", 1024.0},
	    {"mi", 1048576.0},
	    {"mib", 1048576.0},
	    {"gi", 1073741824.0},
	    {"gib", 1073741824.0},
	    {"ti", 1099511627776.0},
	    {"tib", 1099511627776.0},
	    {"pi", 1125899906842624.0},
	    {"pib", 1125899906842624.0},
	    {"ei", 1152921504606846976.0},
	    {"eib", 1152921504606846976.0}};

	static constexpr double kMaxValue = 18446744073709551615.0;

	auto cleanStr = trimSpaces(str);
	size_t numberEndPosition = 0;
	double value = kInvalidConversion;

	try {
		value = std::stod(cleanStr, &numberEndPosition);
	} catch (const std::invalid_argument &e) { return kInvalidConversion; }

	if (numberEndPosition < cleanStr.size()) {
		// Remove leading and trailing spaces
		std::string unit = trimSpaces(cleanStr.substr(numberEndPosition));

		// Convert to lowercase to be case-insensitive
		std::transform(
		    unit.begin(), unit.end(), unit.begin(),
		    [](unsigned char character) { return std::tolower(character); });

		if (units.contains(unit)) {
			value *= units.at(unit);
		} else {
			return kInvalidConversion;
		}
	}

	if (value > kMaxValue) { return kInvalidConversion; }

	return static_cast<int64_t>(std::round(value));
}

#define STR_TO_int(x) return strtol(x,NULL,0)
#define STR_TO_int32(x) return strtol(x,NULL,0)
#define STR_TO_uint32(x) return strtoul(x,NULL,0)
#define STR_TO_int64(x) return strtoll(x,NULL,0)
#define STR_TO_uint64(x) return strtoull(x,NULL,0)
#define STR_TO_double(x) return strtod(x,NULL)
#define STR_TO_charptr(x) { \
	char* _cfg_ret_tmp = strdup(x); \
	passert(_cfg_ret_tmp); \
	return _cfg_ret_tmp; \
}
#define STR_TO_string(x) return std::string(x)

#define COPY_int(x) return x;
#define COPY_int32(x) return x;
#define COPY_uint32(x) return x;
#define COPY_int64(x) return x;
#define COPY_uint64(x) return x;
#define COPY_double(x) return x;
#define COPY_charptr(x) { \
	char* _cfg_ret_tmp = strdup(x); \
	passert(_cfg_ret_tmp); \
	return _cfg_ret_tmp; \
}
#define COPY_string(x) return x;

#define TOPRINTF_int(x) x
#define TOPRINTF_int32(x) x
#define TOPRINTF_uint32(x) x
#define TOPRINTF_int64(x) x
#define TOPRINTF_uint64(x) x
#define TOPRINTF_double(x) x
#define TOPRINTF_charptr(x) x
#define TOPRINTF_string(x) x.c_str()

#define _CONFIG_GEN_FUNCTION(fname,type,convname,format) \
type cfg_get##fname(const char *name, const type def) { \
	if (configParameters.count(name) > 0) { \
		STR_TO_##convname(configParameters[name].c_str()); \
	} \
	if (logundefined) { \
		safs::log_info("config: using default value for option '{}' - '{}'", \
				name,TOPRINTF_##convname(def)); \
	} \
	COPY_##convname(def) \
}

_CONFIG_GEN_FUNCTION(str,char*,charptr,"%s")
_CONFIG_GEN_FUNCTION(string,std::string,string,"%s")
_CONFIG_GEN_FUNCTION(num,int,int,"%d")
_CONFIG_GEN_FUNCTION(int8,int8_t,int32,"%" PRId8)
_CONFIG_GEN_FUNCTION(uint8,uint8_t,uint32,"%" PRIu8)
_CONFIG_GEN_FUNCTION(int16,int16_t,int32,"%" PRId16)
_CONFIG_GEN_FUNCTION(uint16,uint16_t,uint32,"%" PRIu16)
_CONFIG_GEN_FUNCTION(int32,int32_t,int32,"%" PRId32)
_CONFIG_GEN_FUNCTION(uint32,uint32_t,uint32,"%" PRIu32)
_CONFIG_GEN_FUNCTION(int64,int64_t,int64,"%" PRId64)
_CONFIG_GEN_FUNCTION(uint64,uint64_t,uint64,"%" PRIu64)
_CONFIG_GEN_FUNCTION(double,double,double,"%f")
