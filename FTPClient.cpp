#include "FTPClient.h"

// helper macro
#define CLIENT_SEND(fmt, ...)                        \
  do                                                 \
  {                                                  \
    FTP_DEBUG_MSG(">>> " fmt, ##__VA_ARGS__);        \
    control.printf_P(PSTR(fmt "\n"), ##__VA_ARGS__); \
  } while (0)

FTPClient::FTPClient(FS &_FSImplementation) : FTPCommon(_FSImplementation)
{
}

void FTPClient::begin(const ServerInfo &theServer)
{
  _server = &theServer;
}

const FTPClient::Status &FTPClient::transfer(const String &localFileName, const String &remoteFileName, TransferType direction)
{
  _serverStatus.result = PROGRESS;
  if (ftpState >= cIdle)
  {
    _remoteFileName = remoteFileName;
    _direction = direction;

    if (direction & FTP_GET)
      file = THEFS.open(localFileName, "w");
    else if (direction & FTP_PUT)
      file = THEFS.open(localFileName, "r");

    if (!file)
    {
      _serverStatus.result = ERROR;
      _serverStatus.code = 65530;
      _serverStatus.desc = F("Local file error");
    }
    else
    {
      ftpState = cConnect;
      if (direction & 0x80)
      {
        while (ftpState <= cQuit)
        {
          handleFTP();
          delay(25);
        }
      }
    }
  }
  else
  {
    // return error code with status "in PROGRESS"
    _serverStatus.code = 65529;
  }
  return _serverStatus;
}

const FTPClient::Status &FTPClient::check()
{
  return _serverStatus;
}

void FTPClient::handleFTP()
{
  if (_server == nullptr)
  {
    _serverStatus.result = TransferResult::ERROR;
    _serverStatus.code = 65535;
    _serverStatus.desc = F("begin() not called");
  }
  else if (ftpState > cIdle)
  {
    _serverStatus.result = TransferResult::ERROR;
  }
  else if (cConnect == ftpState)
  {
    _serverStatus.code = 65534;
    _serverStatus.desc = F("No connection to FTP server");
    if (controlConnect())
    {
      FTP_DEBUG_MSG("Connection to %s:%u established", control.remoteIP().toString().c_str(), control.remotePort());
      _serverStatus.result = TransferResult::PROGRESS;
      ftpState = cGreet;
    }
    else
    {
      ftpState = cError;
    }
  }
  else if (cGreet == ftpState)
  {
    if (waitFor(220 /* 220 (vsFTPd version) */, F("No server greeting")))
    {
      CLIENT_SEND("USER %s", _server->login.c_str());
      ftpState = cUser;
    }
  }
  else if (cUser == ftpState)
  {
    if (waitFor(331 /* 331 Password */))
    {
      CLIENT_SEND("PASS %s", _server->password.c_str());
      ftpState = cPassword;
    }
  }
  else if (cPassword == ftpState)
  {
    if (waitFor(230 /* 230 Login successful*/))
    {
      CLIENT_SEND("PASV");
      ftpState = cPassive;
    }
  }
  else if (cPassive == ftpState)
  {
    if (waitFor(227 /* 227 Entering Passive Mode (ip,ip,ip,ip,port,port) */))
    {
      bool parseOK = false;
      // find ()
      uint8_t bracketOpen = _serverStatus.desc.indexOf(F("("));
      uint8_t bracketClose = _serverStatus.desc.indexOf(F(")"));
      if (bracketOpen && (bracketClose > bracketOpen))
      {
        FTP_DEBUG_MSG("Parsing PASV response %s", _serverStatus.desc.c_str());
        _serverStatus.desc[bracketClose] = '\0';
        if (parseDataIpPort(_serverStatus.desc.c_str() + bracketOpen + 1))
        {
          // catch ip=0.0.0.0 and replace with the control.remoteIP()
          if (dataIP.toString() == F("0.0.0.0"))
          {
            dataIP = control.remoteIP();
          }
          parseOK = true;
          ftpState = cData;
        }
      }
      if (!parseOK)
      {
        _serverStatus.code = 65533;
        _serverStatus.desc = F("FTP server response not understood.");
      }
    }
  }
  else if (cData == ftpState)
  {
    // open data connection
    if (dataConnect() < 0)
    {
      _serverStatus.code = 65532;
      _serverStatus.desc = F("No data connection to FTP server");
      ftpState = cError;
    }
    else
    {
      FTP_DEBUG_MSG("Data connection to %s:%u established", data.remoteIP().toString().c_str(), data.remotePort());
      millisBeginTrans = millis();
      bytesTransfered = 0;
      ftpState = cTransfer;
      if (_direction & FTP_PUT_NONBLOCKING)
      {
        CLIENT_SEND("STOR %s", _remoteFileName.c_str());
        allocateBuffer(file.size());
      }
      else if (_direction & FTP_GET_NONBLOCKING)
      {
        CLIENT_SEND("RETR %s", _remoteFileName.c_str());
        allocateBuffer(2048);
      }
    }
  }
  else if (cTransfer == ftpState)
  {
    bool res = true;
    if (_direction & FTP_PUT_NONBLOCKING)
    {
      res = doFiletoNetwork();
    }
    else
    {
      res = doNetworkToFile();
    }
    if (!res || !data.connected())
    {
      ftpState = cFinish;
    }
  }
  else if (cFinish == ftpState)
  {
    closeTransfer();
    ftpState = cQuit;
  }
  else if (cQuit == ftpState)
  {
    CLIENT_SEND("QUIT");
    _serverStatus.result = OK;
    ftpState = cIdle;
  }
  else if (cIdle == ftpState)
  {
    stop();
  }
}

int8_t FTPClient::controlConnect()
{
  if (_server->validateCA)
  {
    FTP_DEBUG_MSG("Ignoring CA verification - FTP only");
  }
  control.connect(_server->servername, _server->port);
  FTP_DEBUG_MSG("Connection to %s:%d ... %S", _server->servername.c_str(), _server->port, control.connected() ? PSTR("OK") : PSTR("failed"));
  if (control.connected())
    return 1;
  return -1;
}

bool FTPClient::waitFor(const uint16_t respCode, const __FlashStringHelper *errorString, uint16_t timeOut)
{
  // initalize waiting
  if (0 == waitUntil)
  {
    waitUntil = millis();
    waitUntil += timeOut;
    _serverStatus.desc.clear();
  }
  else
  {
    // timeout
    if ((int32_t)(millis() - waitUntil) >= 0)
    {
      FTP_DEBUG_MSG("Waiting for code %u - timeout!", respCode);
      _serverStatus.code = 65535;
      if (errorString)
      {
        _serverStatus.desc = errorString;
      }
      else
      {
        _serverStatus.desc = F("timeout");
      }
      ftpState = cTimeout;
      waitUntil = 0;
      return false;
    }

    // check for bytes from the client
    while (control.available())
    {
      char c = control.read();
      //FTP_DEBUG_MSG("readChar() line='%s' <= %c", _serverStatus.desc.c_str(), c);
      if (c == '\n' || c == '\r')
      {
        // filter out empty lines
        _serverStatus.desc.trim();
        if (0 == _serverStatus.desc.length())
          continue;

        // line complete, evaluate code
        _serverStatus.code = strtol(_serverStatus.desc.c_str(), NULL, 0);
        if (respCode != _serverStatus.code)
        {
          ftpState = cError;
          FTP_DEBUG_MSG("Waiting for code %u but SMTP server replies: %s", respCode, _serverStatus.desc.c_str());
        }
        else
        {
          FTP_DEBUG_MSG("Waiting for code %u success, SMTP server replies: %s", respCode, _serverStatus.desc.c_str());
        }

        waitUntil = 0;
        return (respCode == _serverStatus.code);
      }
      else
      {
        // just add the char
        _serverStatus.desc += c;
      }
    }
  }
  return false;
}

/*
bool SMTPSSender::connect()
{
  client = new WiFiClientSecure();
  if (NULL == client)
    return false;

  DEBUG_MSG("%SCA validation!", _server->validateCA ? PSTR("") : PSTR("NO "));

  if (_server->validateCA == false)
  {
    // disable CA checks
    reinterpret_cast<WiFiClientSecure *>(client)->setInsecure();
  }

  // Determine if MFLN is supported by a server
  // if it returns true, use the ::setBufferSizes(rx, tx) to shrink
  // the needed BearSSL memory while staying within protocol limits.
  bool mfln = reinterpret_cast<WiFiClientSecure *>(client)->probeMaxFragmentLength(_server->servername, _server->port, 512);

  DEBUG_MSG("MFLN %Ssupported", mfln ? PSTR("") : PSTR("un"));

  if (mfln)
  {
    reinterpret_cast<WiFiClientSecure *>(client)->setBufferSizes(512, 512);
  }

  reinterpret_cast<WiFiClientSecure *>(client)->connect(_server->servername, _server->port);
  return reinterpret_cast<WiFiClientSecure *>(client)->connected();
}

*/