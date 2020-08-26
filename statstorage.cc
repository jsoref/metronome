#include "statstorage.hh"
#include <string>
#include "metromisc.hh"
#include <sys/stat.h>
#include <algorithm>
#include <iostream>
#include <dirent.h>
#include <boost/algorithm/string.hpp>
using namespace std;

StatStorage::StatStorage(const string& fname) : d_root(fname)
{
  if(regcomp(&d_preg, "^[A-Za-z0-9_.-]+$", REG_NOSUB|REG_EXTENDED))
    throw runtime_error("Regular expression did not compile");
}

StatStorage::~StatStorage()
{
  regfree(&d_preg);
}

unsigned int StatStorage::getWeekNum(uint32_t t)
{
  return t/(7*86400);
}

string StatStorage::makeFilename(const string& name, uint32_t timestamp)
{
  return d_root+"/"+name+"."+to_string(getWeekNum(timestamp));
}

void StatStorage::store(const string& name, uint32_t timestamp, float value)
{
  if(name.find("/") != string::npos)
    return;

  if (regexec(&d_preg, name.c_str(), 0, NULL, 0) == REG_NOMATCH)
    return;

  string fname=makeFilename(name, timestamp);
  auto fp = std::unique_ptr<FILE, int(*)(FILE*)>(fopen(fname.c_str(), "a"), fclose);
  if(!fp)
    unixDie("Opening '"+fname+"'");
  StatStorage::Val val({timestamp, value});  
  if(fwrite(&val, 1, sizeof(val), fp.get()) != sizeof(val)) {
    throw runtime_error("Failed to store datum in "+fname+", may be corrupted now");
  }
}

void StatStorage::store(const string& name, const vector<Datum>& data)
{
  if(name.find("/") != string::npos)
    return;

  unsigned int weekno=0;
  string fname;
  std::unique_ptr<FILE, int(*)(FILE*)> fp{nullptr, fclose};
  for(const auto& d: data) {
    if(getWeekNum(d.timestamp) != weekno) {
      weekno=getWeekNum(d.timestamp);
      fname=makeFilename(name, d.timestamp);
      fp = std::unique_ptr<FILE, int(*)(FILE*)>(fopen(fname.c_str(), "a"), fclose);
      if(!fp)
	unixDie("Opening '"+fname+"'");
    }
    StatStorage::Val val({d.timestamp, d.value});  
    if(fwrite(&val, 1, sizeof(val), fp.get()) != sizeof(val)) {
      throw runtime_error("Failed to store datum in "+fname+", may be corrupted now");
    }
  }
}


static uint64_t filesize(int fd)
{
  struct stat buf;
  if(!fstat(fd, &buf)) {
    return buf.st_size;
  }
  return 0;
}

vector<string> StatStorage::getMetrics()
{
  auto dir = std::unique_ptr<DIR, int(*)(DIR*)>(opendir(d_root.c_str()), closedir);
  if (!dir) {
    unixDie("Listing metrics from statistics storage");
  }

  vector<string> ret;
  for(;;) {
    struct dirent* result = readdir(dir.get());
    if (!result) {
      break;
    }
    if (result->d_name[0] != '.') {
      char *p;
      for(p=result->d_name + strlen(result->d_name) - 1; p !=result->d_name && *p!='.'; --p);
      *p='\0';
      
      if (*result->d_name) {
	ret.push_back(result->d_name);
      }
    }
  }
  sort(ret.begin(), ret.end());
  auto newend=unique(ret.begin(), ret.end());
  ret.resize(distance(ret.begin(), newend));
  return ret;
}

vector<StatStorage::Val> StatStorage::retrieveVals(const std::string& name)
{
  auto dir = std::unique_ptr<DIR, int(*)(DIR*)>(opendir(d_root.c_str()), closedir);
  if (!dir) {
    unixDie("Listing metrics from statistics storage");
  }

  vector<string> files;
  for(;;) {
    struct dirent* result = readdir(dir.get());
    if (!result) {
      break;
    }
    if (boost::starts_with(result->d_name, name+".") || result->d_name==name) {
      files.push_back(result->d_name);
    }
  }

  vector<StatStorage::Val> ret;
  for(const auto& f: files) {
    retrieveAllFromFile(d_root+"/"+f, &ret);
  }
    
  return ret;
}

void StatStorage::retrieveAllFromFile(const std::string& fname, vector<StatStorage::Val>* values)
{
  auto fp = std::unique_ptr<FILE, int(*)(FILE*)>(fopen(fname.c_str(), "r"), fclose);
  if(!fp) {
    if(errno!=ENOENT)
      unixDie("Opening '"+fname+"'");
    return;
  }
  auto size = filesize(fileno(fp.get()));
  auto numEntries = size/sizeof(StatStorage::Val);
  auto oldsize=values->size();
  values->resize(oldsize + numEntries);
  //  cerr<<"Filesize: "<<size<<", "<<values.size()<<endl;
  if(fread(&(*values)[oldsize], sizeof(StatStorage::Val), numEntries, fp.get()) != numEntries) {
    unixDie("Reading from '"+fname+"'");
  }
}
			 

vector<StatStorage::Val> StatStorage::retrieveVals(const std::string& name, uint32_t begin, uint32_t end)
{
  vector<StatStorage::Val> values;

  if(name.find("/") != string::npos)
    return values;

  for(uint32_t t = begin; t < end + 7*86400; t += 7*86400) {
    string fname=makeFilename(name, t);
    retrieveAllFromFile(fname, &values);

  }
  if(!is_sorted(values.begin(), values.end()))
    sort(values.begin(), values.end());

  return values;
}

vector<StatStorage::Datum> StatStorage::retrieve(const std::string& name)
{
  auto vals = retrieveVals(name);
  vector<Datum> ret;
  for(const auto& val : vals) {
    ret.push_back({val.timestamp, val.value});
  }
  return ret;
}

vector<StatStorage::Datum> StatStorage::retrieve(const std::string& name, time_t begin, time_t end, int number)
{
  vector<Val> values=retrieveVals(name, begin, end);
  vector<Datum> ret;
  if(values.empty())
    return ret;
  auto beginIter = lower_bound(values.begin(), values.end(), (int64_t)begin);
  auto endIter = lower_bound(values.begin(), values.end(), (int64_t)end);
  
  ret.reserve(endIter- beginIter);
  for(auto iter = beginIter; iter != endIter; ++iter) {
    ret.push_back({iter->timestamp, iter->value});
  }
  return ret;
}
