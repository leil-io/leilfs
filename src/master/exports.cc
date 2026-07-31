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

#include "master/exports.h"

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <syslog.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/cwrap.h"
#include "common/datapack.h"
#include "common/event_loop.h"
#include "common/exceptions.h"
#include "common/md5.h"
#include "config/cfg.h"
#include "slogger/slogger.h"

static std::vector<ExportEntry> exportRecords;
static std::string ExportsFileName;

static char* exports_strsep(char **stringp, const char *delim) {
	char *s;
	const char *spanp;
	int c, sc;
	char *tok;

	s = *stringp;
	if (s==NULL) {
		return NULL;
	}
	tok = s;
	while (1) {
		c = *s++;
		spanp = delim;
		do {
			if ((sc=*spanp++)==c) {
				if (c==0) {
					s = NULL;
				} else {
					s[-1] = 0;
				}
				*stringp = s;
				return tok;
			}
		} while (sc!=0);
	}
	return NULL;    // unreachable
}


uint32_t exports_info_size(uint8_t versmode) {
	uint32_t size = 0;

	for (const auto &e : exportRecords) {
		size += e.serialized_size(versmode);
	}

	return size;
}

void exports_info_data(uint8_t versmode,uint8_t *buff) {
	for (const auto &e : exportRecords) {
		put32bit(&buff, e.fromip);
		put32bit(&buff, e.toip);
		if (e.meta) {
			put32bit(&buff, 1);
			put8bit(&buff, '.');
		} else {
			put32bit(&buff, e.pleng + 1);
			put8bit(&buff, '/');
			if (e.pleng > 0) {
				memcpy(buff, e.path.data(), e.pleng);
				buff += e.pleng;
			}
		}
		put32bit(&buff, e.minversion);
		put8bit(&buff, (e.alldirs ? 1 : 0) + (e.needpassword ? 2 : 0));
		put8bit(&buff, e.sesflags);
		put32bit(&buff, e.rootuid);
		put32bit(&buff, e.rootgid);
		put32bit(&buff, e.mapalluid);
		put32bit(&buff, e.mapallgid);
		if (versmode) {
			put8bit(&buff, e.mingoal);
			put8bit(&buff, e.maxgoal);
			put32bit(&buff, e.mintrashtime);
			put32bit(&buff, e.maxtrashtime);
		}
	}
}

uint8_t exports_check(uint32_t ip, uint32_t version, uint8_t meta,
		const uint8_t *path, const uint8_t rndcode[32],
		const uint8_t *passcode, uint8_t *sesflags,
		uint32_t *rootuid, uint32_t *rootgid, uint32_t *mapalluid,
		uint32_t *mapallgid, uint8_t *mingoal, uint8_t *maxgoal,
		uint32_t *mintrashtime, uint32_t *maxtrashtime) {
	const uint8_t *p;
	uint32_t pleng,i;
	uint8_t rndstate;
	int ok,nopass;
	md5ctx md5c;
	std::vector<uint8_t> entrydigest(kDefaultMd5DigestSize);
	ExportEntry *foundEntry = nullptr;

	if (meta==0) {
		p = path;
		while (*p=='/') {
			p++;
		}
		pleng = 0;
		while (p[pleng]) {
			pleng++;
		}
		while (pleng>0 && p[pleng-1]=='/') {
			pleng--;
		}
	} else {
		p = NULL;
		pleng = 0;
	}
	rndstate=0;
	if (rndcode!=NULL) {
		for (i=0 ; i<32 ; i++) {
			rndstate|=rndcode[i];
		}
	}

	nopass=0;
	for (auto &e : exportRecords) {
		ok = 0;
		if (ip >= e.fromip && ip <= e.toip && version >= e.minversion && meta == e.meta) {
			// path check
			if (meta) {     // no path in META
				ok=1;
			} else {
				if (e.pleng == 0) {  // root dir
					if (pleng == 0) {
						ok = 1;
					} else if (e.alldirs) {
						ok = 1;
					}
				} else {
					if (pleng == e.pleng && memcmp(p, e.path.data(), pleng) == 0) {
						ok = 1;
					} else if (e.alldirs && pleng > e.pleng && p[e.pleng] == '/' &&
					           memcmp(p, e.path.data(), e.pleng) == 0) {
						ok = 1;
					}
				}
			}

			if (ok && e.needpassword) {
				if (rndstate==0 || rndcode==NULL || passcode==NULL) {
					ok=0;
					nopass=1;
				} else {
					md5_init(&md5c);
					md5_update(&md5c, rndcode, kDefaultMd5DigestSize);
					md5_update(&md5c, e.passworddigest.data(), kDefaultMd5DigestSize);
					md5_update(&md5c, rndcode + kDefaultMd5DigestSize, kDefaultMd5DigestSize);
					md5_final(entrydigest.data(), &md5c);
					if (memcmp(entrydigest.data(),passcode,kDefaultMd5DigestSize)!=0) {
						ok=0;
						nopass=1;
					}
				}
			}
		}
		if (ok) {
			if (foundEntry == nullptr) {
				foundEntry = &e;
			} else {
				if ((e.sesflags & SESFLAG_READONLY) == 0 &&
					(foundEntry->sesflags & SESFLAG_READONLY) != 0) {  // prefer rw to ro
					foundEntry = &e;
				} else if (e.rootuid == 0 &&
						   foundEntry->rootuid != 0) {  // prefer root not restricted to restricted
					foundEntry = &e;
				} else if ((e.sesflags & SESFLAG_ALLCANCHANGEQUOTA) != 0 &&
						   (foundEntry->sesflags & SESFLAG_ALLCANCHANGEQUOTA) ==
							   0) {  // prefer lines with more privileges
					foundEntry = &e;
				} else if (e.needpassword == 1 &&
						   foundEntry->needpassword == 0) {  // prefer lines with passwords
					foundEntry = &e;
				} else if (e.pleng > foundEntry->pleng) {  // prefer more accurate path
					foundEntry = &e;
				}
			}
		}
	}
	if (foundEntry == nullptr) {
		if (nopass) {
			if (rndstate==0 || rndcode==NULL || passcode==NULL) {
				return SAUNAFS_ERROR_NOPASSWORD;
			} else {
				return SAUNAFS_ERROR_BADPASSWORD;
			}
		}
		return SAUNAFS_ERROR_EACCES;
	}
	*sesflags = foundEntry->sesflags;
	*rootuid = foundEntry->rootuid;
	*rootgid = foundEntry->rootgid;
	*mapalluid = foundEntry->mapalluid;
	*mapallgid = foundEntry->mapallgid;
	*mingoal = foundEntry->mingoal;
	*maxgoal = foundEntry->maxgoal;
	*mintrashtime = foundEntry->mintrashtime;
	*maxtrashtime = foundEntry->maxtrashtime;
	return SAUNAFS_STATUS_OK;
}

// format:
// ip[/bits]    path    options
//
// options:
//  readonly
//  maproot=uid[:gid]
//  alldirs
//  md5pass=md5(password)
//  password=password
//  dynamicip
//  ignoregid
//  allcanchangequota
//  mingoal=#
//  maxgoal=#
//  mintrashtime=[#w][#d][#h][#m][#[s]]
//  maxtrashtime=[#w][#d][#h][#m][#[s]]
//
// ip[/bits] can be '*' (same as 0.0.0.0/0)
//
// default:
// *    /       rw,alldirs,maproot=0

static int exports_parsenet(char *net,uint32_t *fromip,uint32_t *toip) {
	uint32_t ip,i,octet;
	if (net[0]=='*' && net[1]==0) {
		*fromip = 0;
		*toip = 0xFFFFFFFFU;
		return 0;
	}
	ip=0;
	for (i=0 ; i<4; i++) {
		if (*net>='0' && *net<='9') {
			octet=0;
			while (*net>='0' && *net<='9') {
				octet*=10;
				octet+=*net-'0';
				net++;
				if (octet>255) {
					return -1;
				}
			}
		} else {
			return -1;
		}
		if (i<3) {
			if (*net!='.') {
				return -1;
			}
			net++;
		}
		ip*=256;
		ip+=octet;
	}
	if (*net==0) {
		*fromip = ip;
		*toip = ip;
		return 0;
	}
	if (*net=='/') {        // ip/bits and ip/mask
		*fromip = ip;
		ip=0;
		net++;
		for (i=0 ; i<4; i++) {
			if (*net>='0' && *net<='9') {
				octet=0;
				while (*net>='0' && *net<='9') {
					octet*=10;
					octet+=*net-'0';
					net++;
					if (octet>255) {
						return -1;
					}
				}
			} else {
				return -1;
			}
			if (i==0 && *net==0 && octet<=32) {     // bits -> convert to mask and skip rest of loop
				ip = 0xFFFFFFFF;
				if (octet<32) {
					ip<<=32-octet;
				}
				break;
			}
			if (i<3) {
				if (*net!='.') {
					return -1;
				}
				net++;
			}
			ip*=256;
			ip+=octet;
		}
		if (*net!=0) {
			return -1;
		}
		*fromip &= ip;
		*toip = *fromip | (ip ^ 0xFFFFFFFFU);
		return 0;
	}
	if (*net=='-') {        // ip1-ip2
		*fromip = ip;
		ip=0;
		net++;
		for (i=0 ; i<4; i++) {
			if (*net>='0' && *net<='9') {
				octet=0;
				while (*net>='0' && *net<='9') {
					octet*=10;
					octet+=*net-'0';
					net++;
					if (octet>255) {
						return -1;
					}
				}
			} else {
				return -1;
			}
			if (i<3) {
				if (*net!='.') {
					return -1;
				}
				net++;
			}
			ip*=256;
			ip+=octet;
		}
		if (*net!=0) {
			return -1;
		}
		*toip = ip;
		return 0;
	}
	return -1;
}

static int exports_parsegoal(char *goalstr,uint8_t *goal) {
	if (*goalstr < '1' || *goalstr > '9') {
		// the string does not begin with a number or is empty
		return -1;
	}
	char *end = nullptr;
	auto value = strtol(goalstr, &end, 10);
	if (*end != '\0') {
		// not the whole string was used by strtol
		return -1;
	}
	if (value > UINT8_MAX || !GoalId::isValid(value)) {
		return -1;
	}
	*goal = value;
	return 0;
}

// # | [#w][#d][#h][#m][#s]
static int exports_parsetime(char *timestr,uint32_t *time) {
	uint64_t t;
	uint64_t tp;
	uint8_t bits;
	char *p;
	for (p=timestr ; *p ; p++) {
		if (*p>='a' && *p<='z') {
			*p -= ('a'-'A');
		}
	}
	t = 0;
	bits = 0;
	while (1) {
		if (*timestr<'0' || *timestr>'9') {
			return -1;
		}
		tp = 0;
		while (*timestr>='0' && *timestr<='9') {
			tp *= 10;
			tp += *timestr-'0';
			timestr++;
			if (tp>UINT64_C(0x100000000)) {
				return -1;
			}
		}
		switch (*timestr) {
		case 'W':
			if (bits&0x1F) {
				return -1;
			}
			bits |= 0x10;
			tp *= 604800;
			timestr++;
			break;
		case 'D':
			if (bits&0x0F) {
				return -1;
			}
			bits |= 0x08;
			tp *= 86400;
			timestr++;
			break;
		case 'H':
			if (bits&0x07) {
				return -1;
			}
			bits |= 0x04;
			tp *= 3600;
			timestr++;
			break;
		case 'M':
			if (bits&0x03) {
				return -1;
			}
			bits |= 0x02;
			tp *= 60;
			timestr++;
			break;
		case 'S':
			if (bits&0x01) {
				return -1;
			}
			bits |= 0x01;
			timestr++;
			break;
		case '\0':
			if (bits) {
				return -1;
			}
			break;
		default:
			return -1;
		}
		t += tp;
		if (t>UINT64_C(0x100000000)) {
			return -1;
		}
		if (*timestr=='\0') {
			*time = t;
			return 0;
		}
	}
	return -1;      // unreachable
}

// x | x.y | x.y.z -> (x<<16 + y<<8 + z)
static int exports_parseversion(char *verstr,uint32_t *version) {
	uint32_t vp;
	if (*verstr<'0' || *verstr>'9') {
		return -1;
	}
	vp=0;
	while (*verstr>='0' && *verstr<='9') {
		vp*=10;
		vp+=*verstr-'0';
		verstr++;
	}
	if (vp>255 || (*verstr!='.' && *verstr)) {
		return -1;
	}
	*version = vp<<16;
	if (*verstr==0) {
		return 0;
	}
	verstr++;
	if (*verstr<'0' || *verstr>'9') {
		return -1;
	}
	vp=0;
	while (*verstr>='0' && *verstr<='9') {
		vp*=10;
		vp+=*verstr-'0';
		verstr++;
	}
	if (vp>255 || (*verstr!='.' && *verstr)) {
		return -1;
	}
	*version += vp<<8;
	if (*verstr==0) {
		return 0;
	}
	verstr++;
	if (*verstr<'0' || *verstr>'9') {
		return -1;
	}
	vp=0;
	while (*verstr>='0' && *verstr<='9') {
		vp*=10;
		vp+=*verstr-'0';
		verstr++;
	}
	if (vp>255 || *verstr) {
		return -1;
	}
	*version += vp;
	return 0;
}

static int exports_parseuidgid(char *maproot,uint32_t lineno,uint32_t *ruid,uint32_t *rgid) {
	char *uptr,*gptr,*eptr;
	struct group *grrec,grp;
	struct passwd *pwrec,pwd;
	char pwgrbuff[16384];
	uint32_t uid,gid;
	int gidok;

	uptr = maproot;
	gptr = maproot;
	while (*gptr && *gptr!=':') {
		gptr++;
	}

	if (*gptr==':') {
		*gptr = 0;
		gid = 0;
		eptr = gptr+1;
		while (*eptr>='0' && *eptr<='9') {
			gid*=10;
			gid+=*eptr-'0';
			eptr++;
		}
		if (*eptr!=0) { // not only digits - treat it as a groupname
			getgrnam_r(gptr+1,&grp,pwgrbuff,16384,&grrec);
			if (grrec==NULL) {
				safs_pretty_syslog(LOG_WARNING,"sfsexports/maproot: can't find group named '%s' defined in line: %" PRIu32,gptr+1,lineno);
				return -1;
			}
			gid = grrec->gr_gid;
		}
		gidok = 1;
	} else {
		gidok = 0;
		gid = 0;
		gptr = NULL;
	}

	uid = 0;
	eptr = uptr;
	while (*eptr>='0' && *eptr<='9') {
		uid*=10;
		uid+=*eptr-'0';
		eptr++;
	}
	if (*eptr!=0) { // not only digits - treat it as a username
		getpwnam_r(uptr,&pwd,pwgrbuff,16384,&pwrec);
		if (pwrec==NULL) {
			safs_pretty_syslog(LOG_WARNING,"sfsexports/maproot: can't find user named '%s' defined in line: %" PRIu32,uptr,lineno);
			return -1;
		}
		*ruid = pwrec->pw_uid;
		if (gidok==0) {
			*rgid = pwrec->pw_gid;
		} else {
			*rgid = gid;
		}
		return 0;
	} else if (gidok==1) {
		*ruid = uid;
		*rgid = gid;
		return 0;
	} else {
		getpwuid_r(uid,&pwd,pwgrbuff,16384,&pwrec);
		if (pwrec==NULL) {
			safs_pretty_syslog(LOG_WARNING,"sfsexports/maproot: can't determine gid, because can't find user with uid %" PRIu32 " defined in line: %" PRIu32,uid,lineno);
			return -1;
		}
		*ruid = pwrec->pw_uid;
		*rgid = pwrec->pw_gid;
		return 0;
	}
	return -1;      // unreachable
}

static int exports_parseoptions(char *opts, uint32_t lineno, ExportEntry &entry) {
	char *p;
	int o;
	md5ctx ctx;

	while ((p=exports_strsep(&opts,","))) {
		o=0;
		switch (*p) {
		case 'r':
			if (strcmp(p,"ro")==0) {
				entry.sesflags |= SESFLAG_READONLY;
				o=1;
			} else if (strcmp(p,"readonly")==0) {
				entry.sesflags |= SESFLAG_READONLY;
				o=1;
			} else if (strcmp(p,"rw")==0) {
				entry.sesflags &= ~SESFLAG_READONLY;
				o=1;
			} else if (strcmp(p,"readwrite")==0) {
				entry.sesflags &= ~SESFLAG_READONLY;
				o=1;
			}
			break;
		case 'i':
			if (strcmp(p,"ignoregid")==0) {
				if (entry.meta) {
					safs_pretty_syslog(LOG_WARNING,"meta option ignored: %s",p);
				} else {
					entry.sesflags |= SESFLAG_IGNOREGID;
				}
				o=1;
			}
			break;
		case 'a':
			if (strcmp(p,"allcanchangequota")==0) {
				if (entry.meta) {
					safs_pretty_syslog(LOG_WARNING,"meta option ignored: %s",p);
				} else {
					entry.sesflags |= SESFLAG_ALLCANCHANGEQUOTA;
				}
				o=1;
			} else if (strcmp(p,"alldirs")==0) {
				if (entry.meta) {
					safs_pretty_syslog(LOG_WARNING,"meta option ignored: %s",p);
				} else {
					entry.alldirs = 1;
				}
				o=1;
			}
			break;
		case 'c':
			if (strcmp(p,"caseinsensitive")==0) {
				entry.sesflags |= SESFLAG_CASEINSENSITIVE;
				o=1;
			}
			break;
		case 'd':
			if (strcmp(p,"dynamicip")==0) {
				entry.sesflags |= SESFLAG_DYNAMICIP;
				o=1;
			}
			break;
		case 'n':
			if (strcmp(p,"nomasterpermcheck")==0) {
				if (entry.meta) {
					safs_pretty_syslog(LOG_WARNING,"meta option ignored: %s",p);
				} else {
					entry.sesflags |= SESFLAG_NOMASTERPERMCHECK;
				}
				o=1;
			} else if (strcmp(p,"nonrootmeta")==0) {
				if (entry.meta) {
					entry.sesflags |= SESFLAG_NONROOTMETA;
				} else {
					safs_pretty_syslog(LOG_WARNING,"non-meta option ignored: %s",p);
				}
				o=1;
			}
			break;
		case 'm':
			if (strncmp(p,"maproot=",8)==0) {
				o=1;
				if (entry.meta) {
					safs_pretty_syslog(LOG_WARNING,"meta option ignored: %s",p);
				} else {
					if (exports_parseuidgid(p + 8, lineno, &entry.rootuid, &entry.rootgid) < 0) {
						return -1;
					}
					entry.rootredefined = 1;
				}
			} else if (strncmp(p,"mapall=",7)==0) {
				o=1;
				if (entry.meta) {
					safs_pretty_syslog(LOG_WARNING,"meta option ignored: %s",p);
				} else {
					if (exports_parseuidgid(p + 7, lineno, &entry.mapalluid, &entry.mapallgid) <
					    0) {
						return -1;
					}
					entry.sesflags |= SESFLAG_MAPALL;
				}
			} else if (strncmp(p,"md5pass=",8)==0) {
				entry.passworddigest.resize(kDefaultMd5DigestSize);
				char *ptr = p+8;
				uint32_t i=0;
				o=1;
				while ((*ptr>='0' && *ptr<='9') || (*ptr>='a' && *ptr<='f') || (*ptr>='A' && *ptr<='F')) {
					ptr++;
					i++;
				}
				if (*ptr==0 && i==32) {
					ptr = p+8;
					for (i = 0; i < kDefaultMd5DigestSize; i++) {
						if (*ptr>='0' && *ptr<='9') {
							entry.passworddigest[i] = (*ptr - '0') << 4;
						} else if (*ptr>='a' && *ptr<='f') {
							entry.passworddigest[i] = (*ptr - 'a' + 10) << 4;
						} else {
							entry.passworddigest[i] = (*ptr - 'A' + 10) << 4;
						}
						ptr++;
						if (*ptr>='0' && *ptr<='9') {
							entry.passworddigest[i] += (*ptr - '0');
						} else if (*ptr>='a' && *ptr<='f') {
							entry.passworddigest[i] += (*ptr - 'a' + 10);
						} else {
							entry.passworddigest[i] += (*ptr - 'A' + 10);
						}
						ptr++;
					}
					entry.needpassword = 1;
				} else {
					safs_pretty_syslog(LOG_WARNING,"sfsexports: incorrect md5pass definition (%s) in line: %" PRIu32,p,lineno);
					return -1;
				}
			} else if (strncmp(p,"minversion=",11)==0) {
				o=1;
				if (exports_parseversion(p + 11, &entry.minversion) < 0) {
					safs_pretty_syslog(LOG_WARNING,"sfsexports: incorrect minversion definition (%s) in line: %" PRIu32,p,lineno);
					return -1;
				}
			} else if (strncmp(p,"mingoal=",8)==0) {
				o=1;
				if (exports_parsegoal(p + 8, &entry.mingoal) < 0) {
					safs_pretty_syslog(LOG_WARNING,"sfsexports: incorrect mingoal definition (%s) in line: %" PRIu32,p,lineno);
					return -1;
				}
				if (entry.mingoal > entry.maxgoal) {
					safs_pretty_syslog(LOG_WARNING,"sfsexports: mingoal>maxgoal in definition (%s) in line: %" PRIu32,p,lineno);
					return -1;
				}
			} else if (strncmp(p,"maxgoal=",8)==0) {
				o=1;
				if (exports_parsegoal(p + 8, &entry.maxgoal) < 0) {
					safs_pretty_syslog(LOG_WARNING,"sfsexports: incorrect maxgoal definition (%s) in line: %" PRIu32,p,lineno);
					return -1;
				}
				if (entry.mingoal > entry.maxgoal) {
					safs_pretty_syslog(LOG_WARNING,"sfsexports: maxgoal<mingoal in definition (%s) in line: %" PRIu32,p,lineno);
					return -1;
				}
			} else if (strncmp(p,"mintrashtime=",13)==0) {
				o=1;
				if (exports_parsetime(p + 13, &entry.mintrashtime) < 0) {
					safs_pretty_syslog(LOG_WARNING,"sfsexports: incorrect mintrashtime definition (%s) in line: %" PRIu32,p,lineno);
					return -1;
				}
				if (entry.mintrashtime > entry.maxtrashtime) {
					safs_pretty_syslog(LOG_WARNING,"sfsexports: mintrashtime>maxtrashtime in definition (%s) in line: %" PRIu32,p,lineno);
					return -1;
				}
			} else if (strncmp(p,"maxtrashtime=",13)==0) {
				o=1;
				if (exports_parsetime(p + 13, &entry.maxtrashtime) < 0) {
					safs_pretty_syslog(LOG_WARNING,"sfsexports: incorrect maxtrashtime definition (%s) in line: %" PRIu32,p,lineno);
					return -1;
				}
				if (entry.mintrashtime > entry.maxtrashtime) {
					safs_pretty_syslog(LOG_WARNING,"sfsexports: maxtrashtime<mintrashtime in definition (%s) in line: %" PRIu32,p,lineno);
					return -1;
				}
			}
			break;
		case 'p':
			if (strncmp(p,"password=",9)==0) {
				entry.passworddigest.resize(kDefaultMd5DigestSize);
				md5_init(&ctx);
				md5_update(&ctx,(uint8_t*)(p+9),strlen(p+9));
				md5_final(entry.passworddigest.data(), &ctx);
				entry.needpassword = 1;
				o=1;
			}
			break;
		}
		if (o==0) {
			safs_pretty_syslog(LOG_WARNING,"sfsexports: unknown option '%s' in line: %" PRIu32 " (ignored)",p,lineno);
		}
	}
	return 0;
}

static int exports_parseline(char *line, uint32_t lineno, ExportEntry &entry) {
	char *net,*path;
	char *p;
	uint32_t pleng;

	p = line;
	while (*p==' ' || *p=='\t') {
		p++;
	}
	net = p;
	while (*p && *p!=' ' && *p!='\t') {
		p++;
	}
	if (*p==0) {
		safs_pretty_syslog(LOG_WARNING,"sfsexports: incomplete definition in line: %" PRIu32,lineno);
		return -1;
	}
	*p=0;
	p++;
	if (exports_parsenet(net, &entry.fromip, &entry.toip) < 0) {
		safs_pretty_syslog(LOG_WARNING,"sfsexports: incorrect ip/network definition in line: %" PRIu32,lineno);
		return -1;
	}

	while (*p==' ' || *p=='\t') {
		p++;
	}
	if (p[0]=='.' && (p[1]==0 || p[1]==' ' || p[1]=='\t')) {
		path = NULL;
		pleng = 0;
		entry.rootuid = 0;
		entry.rootgid = 0;
		entry.meta = 1;
		p++;
	} else {
		while (*p=='/') {
			p++;
		}
		path = p;
		pleng = 0;
		while (*p && *p!=' ' && *p!='\t') {
			p++;
			pleng++;
		}
		while (pleng>0 && path[pleng-1]=='/') {
			pleng--;
		}
	}
	if (*p==0) {
		// no options - use defaults
		entry.pleng = pleng;
		if (pleng>0) {
			entry.path.assign(reinterpret_cast<uint8_t *>(path),
			                  reinterpret_cast<uint8_t *>(path) + pleng);
			entry.path.push_back(0);
		}
		return 0;
	}
	while (*p==' ' || *p=='\t') {
		p++;
	}

	if (exports_parseoptions(p, lineno, entry) < 0) {
		return -1;
	}

	if ((entry.sesflags & SESFLAG_MAPALL) && (entry.rootredefined == 0)) {
		entry.rootuid = entry.mapalluid;
		entry.rootgid = entry.mapallgid;
	}

	entry.pleng = pleng;
	if (pleng>0) {
		entry.path.assign(reinterpret_cast<uint8_t *>(path),
		                  reinterpret_cast<uint8_t *>(path) + pleng);
		entry.path.push_back(0);
	}

	return 0;
}

static void exports_loadexports() {
	const size_t MAX_LINE_LEN = 10000;
	FILE *fd;
	char linebuff[MAX_LINE_LEN];
	uint32_t s,lineno;
	std::vector<ExportEntry> newExports;

	fd = fopen(ExportsFileName.c_str(), "r");
	if (!fd) {
		if (errno == ENOENT) {
			std::string err_msg = std::string("exports "
				"configuration file (") + ExportsFileName +
				") not found - please create one (you can copy "
				APP_EXAMPLES_SUBDIR "/leil-exports.cfg to "
				"get a base configuration)";
			throw InitializeException(err_msg);
		} else {
			std::string err_msg = std::string("can't open exports "
				    "configuration  file (") + ExportsFileName
				    + "):" + errorString(errno);
			throw InitializeException(err_msg);
		}
	}

	for (lineno = 1; fgets(linebuff,MAX_LINE_LEN,fd); lineno++) {
		if (linebuff[0]=='#')
			continue;
		linebuff[MAX_LINE_LEN-1]=0;
		s=strlen(linebuff);
		while (s > 0 && (linebuff[s-1]=='\r' ||
				 linebuff[s-1]=='\n' ||
				 linebuff[s-1]=='\t' ||
				 linebuff[s-1]==' ')) {
			s--;
		}
		if (s>0) {
			linebuff[s]=0;
			ExportEntry entry;
			if (exports_parseline(linebuff, lineno, entry) >= 0) {
				newExports.push_back(std::move(entry));
			}
		}
	}
	if (ferror(fd)) {
		fclose(fd);
		throw InitializeException(
				std::string("can't read exports configuration file (") + ExportsFileName + ")");
	}
	fclose(fd);
	exportRecords = std::move(newExports);
	safs_pretty_syslog(LOG_INFO, "initialized exports from file %s", ExportsFileName.c_str());
}

static void exports_load() {
	const std::string defaultExportsFileName = ETC_PATH "/leil-exports.cfg";
	const std::string legacyExportsFileName = ETC_PATH_LEGACY "/sfsexports.cfg";
	ExportsFileName = cfg_getstring("EXPORTS_FILENAME", defaultExportsFileName);
	if (ExportsFileName == defaultExportsFileName &&
	    access(defaultExportsFileName.c_str(), F_OK) != 0 &&
	    access(legacyExportsFileName.c_str(), F_OK) == 0) {
		safs::log_warn(
		    "using legacy exports configuration file {} because default file {} was not found",
		    legacyExportsFileName.c_str(), defaultExportsFileName.c_str());
		ExportsFileName = legacyExportsFileName;
	}
	exports_loadexports();
}

static void exports_reload(void) {
	try {
		exports_load();
	} catch (Exception& ex) {
		safs_pretty_syslog(LOG_WARNING, "exports not changed because %s", ex.what());
	}
}

void exports_term() {
	exportRecords.clear();
}

int exports_init() {
	exportRecords.clear();
	exports_load();
	if (exportRecords.empty()) {
		// File was empty
		throw InitializeException(std::string("no exports defined in ") + ExportsFileName);
	}
	eventloop_reloadregister(exports_reload);
	eventloop_destructregister(exports_term);
	return 0;
}
