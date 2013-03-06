#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sstream>

#include "timing.h"
#include "converter.h"
#include "procs.h"
#include "config.h"

namespace Converter {
  
  Converter::Converter(){
    fillFFMpegEncoders();
  }
  
  void Converter::fillFFMpegEncoders(){
    std::vector<char*> cmd;
    cmd.reserve(3);
    cmd.push_back((char*)"ffmpeg");
    cmd.push_back((char*)"-encoders");
    cmd.push_back(NULL);
    int outFD = -1;
    Util::Procs::StartPiped("FFMpegInfo", &cmd[0], 0, &outFD, 0);
    while( Util::Procs::isActive("FFMpegInfo")){ Util::sleep(100); }
    FILE * outFile = fdopen( outFD, "r" );
    char * fileBuf = 0;
    size_t fileBufLen = 0;
    while ( !(feof(outFile) || ferror(outFile)) && (getline(&fileBuf, &fileBufLen, outFile) != -1)){
      if (strstr(fileBuf, "aac") || strstr(fileBuf, "AAC")){
        strtok(fileBuf, " \t");
        allCodecs["ffmpeg"][strtok(NULL, " \t")] = "aac";
      }
      if (strstr(fileBuf, "h264") || strstr(fileBuf, "H264")){
        strtok(fileBuf, " \t");
        allCodecs["ffmpeg"][strtok(NULL, " \t")] = "h264";
      }
      if (strstr(fileBuf, "mp3") || strstr(fileBuf, "MP3")){
        strtok(fileBuf, " \t");
        allCodecs["ffmpeg"][strtok(NULL, " \t")] = "mp3";
      }
    }
    fclose( outFile );
  }
  
  converterInfo & Converter::getCodecs(){
    return allCodecs;
  }

  JSON::Value Converter::getEncoders(){
    JSON::Value result;
    for (converterInfo::iterator convIt = allCodecs.begin(); convIt != allCodecs.end(); convIt++){
      for (codecInfo::iterator codIt = convIt->second.begin(); codIt != convIt->second.end(); codIt++){
        result[convIt->first][codIt->first] = codIt->second;
      }
    }
    return result;
  }
  
  JSON::Value Converter::queryPath(std::string myPath){
    std::vector<char*> cmd;
    cmd.reserve(3);
    cmd.push_back((char*)"MistInfo");
    cmd.push_back(NULL);
    cmd.push_back(NULL);
    fprintf( stderr, "Querying %s\n", myPath.c_str());
    JSON::Value result;
    DIR * Dirp = opendir(myPath.c_str());
    struct stat StatBuf;
    if (Dirp){
      dirent * entry;
      while ((entry = readdir(Dirp))){
        if (stat(std::string(myPath + "/" + entry->d_name).c_str(), &StatBuf) == -1){
          continue;
        }
        if ((StatBuf.st_mode & S_IFREG) == 0){
          continue;
        }
        std::string fileName = entry->d_name;
        std::string myPath = std::string(myPath + (myPath[myPath.size()-1] == '/' ? "" : "/") +  entry->d_name);
        cmd[1] = (char*)myPath.c_str();
        int outFD = -1;
        Util::Procs::StartPiped("MistInfo", &cmd[0], 0, &outFD, 0);
        while( Util::Procs::isActive("MistInfo")){ Util::sleep(10); }
        FILE * outFile = fdopen( outFD, "r" );
        char * fileBuf = 0;
        size_t fileBufLen = 0;
        getline(&fileBuf, &fileBufLen, outFile);
        std::string line = fileBuf;
        result[fileName] = JSON::fromString(std::string(fileBuf));
        if ( !result[fileName]){
          result.removeMember(fileName);
        }
        fclose( outFile );
      }
    }
    return result;
  }

  void Converter::startConversion(std::string name, JSON::Value parameters) {
    if ( !parameters.isMember("input")){
      statusHistory[name] = "No input file supplied";
      return;
    }
    if ( !parameters.isMember("output")){
      statusHistory[name] = "No output file supplied";
      return;
    }
    if ( !parameters.isMember("encoder")){
      statusHistory[name] = "No encoder specified";
      return;
    }
    if (allCodecs.find(parameters["encoder"]) == allCodecs.end()){
      statusHistory[name] = "Can not find encoder " + parameters["encoder"];
      return;
    }
    std::stringstream encoderCommand;
    if (parameters["encoder"] == "ffmpeg"){
      encoderCommand << "ffmpeg -i ";
      encoderCommand << parameters["input"].asString() << " ";
      if (parameters.isMember("video")){
        if ( !parameters["video"].isMember("codec") || parameters["video"]["codec"] == "copy"){
          encoderCommand << "-vcodec copy ";
        }else{
          codecInfo::iterator vidCodec = allCodecs["ffmpeg"].find(parameters["video"]["codec"]);
          if (vidCodec == allCodecs["ffmpeg"].end()){
            statusHistory[name] = "Can not find video codec " + parameters["video"]["codec"].asString();
            return;
          }
          encoderCommand << "-vcodec " << vidCodec->first << " ";
          if (parameters["video"].isMember("kfps")){
            encoderCommand << "-r " << parameters["video"]["kfps"].asInt() / 1000 << " "; 
          }
          ///\todo Keyframe interval (different in older and newer versions of ffmpeg?)
        }
      }else{
        encoderCommand << "-vn ";
      }
      if (parameters.isMember("audio")){
        if ( !parameters["audio"].isMember("codec")){
          encoderCommand << "-acodec copy ";
        }else{
          codecInfo::iterator audCodec = allCodecs["ffmpeg"].find(parameters["audio"]["codec"]);
          if (audCodec == allCodecs["ffmpeg"].end()){
            statusHistory[name] = "Can not find audio codec " + parameters["audio"]["codec"].asString();
            return;
          }
          if (audCodec->second == "aac"){
            encoderCommand << "-strict -2 ";
          }
          encoderCommand << "-acodec " << audCodec->first << " ";
          if (parameters["audio"].isMember("samplerate")){
            encoderCommand << "-ar " << parameters["audio"]["samplerate"].asInt() << " ";
          }
        }
      }else{
        encoderCommand << "-an ";
      }
      encoderCommand << "-f flv -";
    }
    Util::Procs::Start(name,encoderCommand.str(),Util::getMyPath() + "MistFLV2DTSC -o " + parameters["output"].asString());
    allConversions[name] = parameters;
  }
  
  void Converter::updateStatus(){
    if (allConversions.size()){
      std::map<std::string,JSON::Value>::iterator cIt;
      bool hasChanged = true;
      while (hasChanged && allConversions.size()){
        hasChanged = false;
        for (cIt = allConversions.begin(); cIt != allConversions.end(); cIt++){
          if (Util::Procs::isActive(cIt->first)){
            continue;
          }
          if (cIt->second["output"].asString().find(".dtsc") != std::string::npos){
            statusHistory[cIt->first] = "Conversion succesful, running DTSCFix";
            Util::Procs::Start(cIt->first+"DTSCFix",Util::getMyPath() + "MistDTSCFix " + cIt->second["output"].asString());
          }else{
            statusHistory[cIt->first] = "Conversion succesful";
          }
          allConversions.erase(cIt);
          hasChanged = true;
          break;
        }
      }
    }
    if(statusHistory.size()){
      std::map<std::string,std::string>::iterator sIt;
      for (sIt = statusHistory.begin(); sIt != statusHistory.end(); sIt++){
        if (statusHistory[sIt->first].find("DTSCFix") != std::string::npos){
          if (Util::Procs::isActive(sIt->first+"DTSCFIX")){
            continue;
          }
          statusHistory[sIt->first] = "Conversion succesful";
        }
      }
    }
  }
  
  JSON::Value Converter::getStatus(){
    updateStatus();
    JSON::Value result;
    if (allConversions.size()){
      for (std::map<std::string,JSON::Value>::iterator cIt = allConversions.begin(); cIt != allConversions.end(); cIt++){
        result[cIt->first] = "Converting";
      }
    }
    if (statusHistory.size()){
      std::map<std::string,std::string>::iterator sIt;
      for (sIt = statusHistory.begin(); sIt != statusHistory.end(); sIt++){
        result[sIt->first] = sIt->second;
      }
    }
    return result;
  }
  
  void Converter::clearStatus(){
    statusHistory.clear();
  }
}