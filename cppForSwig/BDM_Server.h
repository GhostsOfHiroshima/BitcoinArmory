////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2016, goatpig.                                              //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //                                      
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#ifndef _BDM_SERVER_H
#define _BDM_SERVER_H

#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <future>

#include "BitcoinP2p.h"
#include "BlockDataViewer.h"
#include "EncryptionUtils.h"
#include "LedgerEntry.h"
#include "DbHeader.h"
#include "BDV_Notification.h"
#include "BDVCodec.h"
#include "ZeroConf.h"
#include "Server.h"
#include "BtcWallet.h"

#define MAX_CONTENT_LENGTH 1024*1024*1024
#define CALLBACK_EXPIRE_COUNT 5

enum WalletType
{
   TypeWallet,
   TypeLockbox
};

///////////////////////////////////////////////////////////////////////////////
class Callback
{
public:

   virtual ~Callback() = 0;

   virtual void callback(shared_ptr<::Codec_BDVCommand::BDVCallback>) = 0;
   virtual bool isValid(void) = 0;
   virtual void shutdown(void) = 0;
};

///////////////////////////////////////////////////////////////////////////////
class LongPoll : public Callback
{
private:
   mutex mu_;
   atomic<unsigned> count_;
   TimedStack<shared_ptr<::Codec_BDVCommand::BDVCallback>> notificationStack_;

   function<unsigned(void)> isReady_;

private:
   shared_ptr<::google::protobuf::Message> respond_inner(
      vector<shared_ptr<::Codec_BDVCommand::BDVCallback>>& orderVec);

public:
   LongPoll(function<unsigned(void)> isReady) :
      Callback(), isReady_(isReady)
   {
      count_.store(0, memory_order_relaxed);
   }

   shared_ptr<::google::protobuf::Message> respond(
      shared_ptr<::Codec_BDVCommand::BDVCommand>);

   ~LongPoll(void)
   {
      shutdown();
   }

   void shutdown(void)
   {
      //after signaling shutdown, grab the mutex to make sure 
      //all responders threads have terminated
      notificationStack_.terminate();
      unique_lock<mutex> lock(mu_);
   }

   void resetCounter(void)
   {
      count_.store(0, memory_order_relaxed);
   }

   bool isValid(void)
   {
      unique_lock<mutex> lock(mu_, defer_lock);

      if (lock.try_lock())
      {
         auto count = count_.fetch_add(1, memory_order_relaxed) + 1;
         if (count >= CALLBACK_EXPIRE_COUNT)
            return false;
      }

      return true;
   }

   void callback(shared_ptr<::Codec_BDVCommand::BDVCallback> command)
   {
      notificationStack_.push_back(move(command));
   }
};

///////////////////////////////////////////////////////////////////////////////
class WS_Callback : public Callback
{
private:
   const uint64_t bdvID_;

public:
   WS_Callback(const uint64_t& bdvid) :
      bdvID_(bdvid)
   {}

   void callback(shared_ptr<::Codec_BDVCommand::BDVCallback>);
   bool isValid(void) { return true; }
   void shutdown(void) {}
};

///////////////////////////////////////////////////////////////////////////////
class BDV_Server_Object : public BlockDataViewer
{
   friend class Clients;

private: 
   thread initT_;
   unique_ptr<Callback> cb_;

   string bdvID_;
   BlockDataManagerThread* bdmT_;

   map<string, LedgerDelegate> delegateMap_;

   struct walletRegStruct
   {
      shared_ptr<::Codec_BDVCommand::BDVCommand> command_;
      WalletType type_;
   };

   mutex registerWalletMutex_;
   map<string, walletRegStruct> wltRegMap_;

   shared_ptr<promise<bool>> isReadyPromise_;
   shared_future<bool> isReadyFuture_;

   function<void(unique_ptr<BDV_Notification>)> notifLambda_;

private:
   BDV_Server_Object(BDV_Server_Object&) = delete; //no copies
      
   shared_ptr<::google::protobuf::Message> processCommand(
      shared_ptr<::Codec_BDVCommand::BDVCommand>);
   void startThreads(void);

   void registerWallet(shared_ptr<::Codec_BDVCommand::BDVCommand>);
   void registerLockbox(shared_ptr<::Codec_BDVCommand::BDVCommand>);
   void populateWallets(map<string, walletRegStruct>&);

   void resetCounter(void)
   {
      auto longpoll = dynamic_cast<LongPoll*>(cb_.get());
      if (longpoll != nullptr)
         longpoll->resetCounter();
   }

   void setup(void);

   void flagRefresh(
      BDV_refresh refresh, const BinaryData& refreshId,
      unique_ptr<BDV_Notification_ZC> zcPtr);

public:
   BDV_Server_Object(const string& id, BlockDataManagerThread *bdmT);

   ~BDV_Server_Object(void) 
   { 
      haltThreads(); 
   }

   const string& getID(void) const { return bdvID_; }
   void processNotification(shared_ptr<BDV_Notification>);
   void init(void);
   void haltThreads(void);
};

class Clients;

///////////////////////////////////////////////////////////////////////////////
class ZeroConfCallbacks_BDV : public ZeroConfCallbacks
{
private:
   Clients * clientsPtr_;

public:
   ZeroConfCallbacks_BDV(Clients* clientsPtr) :
      clientsPtr_(clientsPtr)
   {}

   set<string> hasScrAddr(const BinaryDataRef&) const;
   void pushZcNotification(ZeroConfContainer::NotificationPacket& packet);
   void errorCallback(
      const string& bdvId, string& errorStr, const string& txHash);
};

///////////////////////////////////////////////////////////////////////////////
class Clients
{
   friend class ZeroConfCallbacks_BDV;

private:
   TransactionalMap<string, shared_ptr<BDV_Server_Object>> BDVs_;
   mutable BlockingStack<bool> gcCommands_;
   BlockDataManagerThread* bdmT_ = nullptr;

   function<void(void)> shutdownCallback_;

   atomic<bool> run_;

   vector<thread> controlThreads_;

   mutable BlockingStack<shared_ptr<BDV_Notification>> outerBDVNotifStack_;
   BlockingStack<shared_ptr<BDV_Notification_Packet>> innerBDVNotifStack_;

private:
   void commandThread(void) const;
   void garbageCollectorThread(void);
   void unregisterAllBDVs(void);
   void bdvMaintenanceLoop(void);
   void bdvMaintenanceThread(void);

public:

   Clients(void)
   {}

   Clients(BlockDataManagerThread* bdmT,
      function<void(void)> shutdownLambda)
   {
      init(bdmT, shutdownLambda);
   }

   void init(BlockDataManagerThread* bdmT,
      function<void(void)> shutdownLambda);

   const shared_ptr<BDV_Server_Object>& get(const string& id) const;
   
   shared_ptr<::google::protobuf::Message> runCommand_FCGI(
      shared_ptr<::Codec_BDVCommand::BDVCommand>);
   shared_ptr<::google::protobuf::Message> runCommand_WS(
      const uint64_t& bdvid, shared_ptr<::Codec_BDVCommand::BDVCommand>);

   void processShutdownCommand(
      shared_ptr<::Codec_BDVCommand::BDVCommand>);
   shared_ptr<::google::protobuf::Message> registerBDV(
      shared_ptr<::Codec_BDVCommand::BDVCommand>, string bdvID);
   void unregisterBDV(const string& bdvId);
   void shutdown(void);
   void exitRequestLoop(void);
};

#endif
