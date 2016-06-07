/*
 * NotebookQueue.cpp
 *
 * Copyright (C) 2009-16 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include "SessionRmdNotebook.hpp"
#include "NotebookQueue.hpp"
#include "NotebookQueueUnit.hpp"
#include "NotebookExec.hpp"
#include "NotebookDocQueue.hpp"

#include <boost/foreach.hpp>

#include <r/RInterface.hpp>

#include <core/Exec.hpp>
#include <core/Thread.hpp>

#include <session/SessionModuleContext.hpp>
#include <session/http/SessionRequest.hpp>

using namespace rstudio::core;

namespace rstudio {
namespace session {
namespace modules {
namespace rmarkdown {
namespace notebook {
namespace {

// represents the global queue of work 
class NotebookQueue
{
public:
   NotebookQueue(const std::string &clientId) :
      clientId_(clientId)
   {
      // launch a thread to process console input
      thread::safeLaunchThread(boost::bind(
               &NotebookQueue::consoleThreadMain, this), &console_);
   }

   Error process()
   {
      // no work if list is empty
      if (queue_.empty())
         return Success();

      // defer if R is currently executing code (we'll initiate processing when
      // the console continues)
      if (r::getGlobalContext()->nextcontext != NULL)
         return Success();

      // if we have a currently executing unit, execute it; otherwise, pop the
      // next unit off the stack
      if (execUnit_)
      {
         if (execUnit_->complete())
         {
            // unit has finished executing; remove it from the queue
            if (queue_.size() > 0)
            {
               boost::shared_ptr<NotebookDocQueue> docQueue = *queue_.begin();
               docQueue->update(execUnit_, QueueDelete, "");
               if (docQueue->complete())
               {
                  queue_.pop_front();
               }
            }

            // clean up current exec unit and execute the next one
            execUnit_ = boost::shared_ptr<NotebookQueueUnit>();
         }
         else
            return executeCurrentUnit();
      }

      return executeNextUnit();
   }

   Error executeNextUnit()
   {
      // no work to do if we have no documents
      if (queue_.empty())
         return Success();

      // get the next execution unit from the current queue
      boost::shared_ptr<NotebookDocQueue> docQueue = *queue_.begin();
      boost::shared_ptr<NotebookQueueUnit> unit = docQueue->firstUnit();

      // establish execution context for the unit
      json::Object options;
      Error error = unit->parseOptions(&options);
      if (error)
         return error;

      // TODO: resolve commit mode
      execContext_ = boost::make_shared<ChunkExecContext>(
         unit->docId(), unit->chunkId(), notebookCtxId(), ExecScopeChunk, options,
         docQueue->pixelWidth(), docQueue->charWidth());
      execContext_->connect();

      execUnit_ = unit;
      executeCurrentUnit();

      return Success();
   }

   Error update(boost::shared_ptr<NotebookQueueUnit> pUnit, QueueOperation op, 
      const std::string& before)
   {
      // find the document queue corresponding to this unit
      BOOST_FOREACH(const boost::shared_ptr<NotebookDocQueue> queue, queue_)
      {
         if (queue->docId() == pUnit->docId())
         {
            queue->update(pUnit, op, before);
            break;
         }
      }

      return Success();
   }

   void add(boost::shared_ptr<NotebookDocQueue> pQueue)
   {
      queue_.push_back(pQueue);
   }

   void consoleThreadMain()
   {
      // main function for thread which receives console input
      std::string input;
      while (input_.deque(&input, boost::posix_time::not_a_date_time))
      {
         // loop back console input request to session -- this allows us to treat 
         // notebook console input exactly as user console input
         core::http::Response response;
         Error error = session::http::sendSessionRequest(
               "/rpc/console_input", input, &response);
         if (error)
            LOG_ERROR(error);
      }
   }

   Error executeCurrentUnit()
   {
      json::Array arr;
      arr.push_back(execUnit_->popExecRange());
      arr.push_back(execUnit_->chunkId());

      // formulate request body
      json::Object rpc;
      rpc["method"] = "console_input";
      rpc["params"] = arr;
      rpc["clientId"] = clientId_;

      // TODO: do we need to update the client ID if a client is disconnected
      // and replaced with a new client?
     
      std::ostringstream oss;
      json::write(rpc, oss);

      input_.enque(oss.str());
      return Success();
   }

   void onConsolePrompt(const std::string& prompt)
   {
      process();
   }

private:
   // the current client id
   std::string clientId_;
   
   // the documents with active queues
   std::list<boost::shared_ptr<NotebookDocQueue> > queue_;

   // the execution context for the currently executing chunk
   boost::shared_ptr<NotebookQueueUnit> execUnit_;
   boost::shared_ptr<ChunkExecContext> execContext_;

   // the thread which submits console input, and the queue which feeds it
   boost::thread console_;
   core::thread::ThreadsafeQueue<std::string> input_;
};

static boost::shared_ptr<NotebookQueue> s_queue;

Error updateExecQueue(const json::JsonRpcRequest& request,
                      json::JsonRpcResponse* pResponse)
{
   json::Object unitJson;
   int op = 0;
   std::string before;
   Error error = json::readParams(request.params, &unitJson, &op, &before);
   if (error)
      return error;

   boost::shared_ptr<NotebookQueueUnit> pUnit = 
      boost::make_shared<NotebookQueueUnit>();
   error = NotebookQueueUnit::fromJson(unitJson, &pUnit);
   if (error)
      return error;

   return s_queue->update(pUnit, static_cast<QueueOperation>(op), before);
}

Error executeNotebookChunks(const json::JsonRpcRequest& request,
                            json::JsonRpcResponse* pResponse)
{
   json::Object docObj;
   Error error = json::readParams(request.params, &docObj);

   boost::shared_ptr<NotebookDocQueue> pQueue = 
      boost::make_shared<NotebookDocQueue>();
   error = NotebookDocQueue::fromJson(docObj, &pQueue);
   if (error)
      return error;

   // create queue if it doesn't exist
   if (!s_queue)
      s_queue = boost::make_shared<NotebookQueue>(request.clientId);

   // add the queue and process immediately
   s_queue->add(pQueue);
   s_queue->process();

   return Success();
}

void onConsolePrompt(const std::string& prompt)
{
   if (s_queue)
   {
      s_queue->onConsolePrompt(prompt);
   }
}

} // anonymous namespace

Error initQueue()
{
   using boost::bind;
   using namespace module_context;

   module_context::events().onConsolePrompt.connect(onConsolePrompt);

   ExecBlock initBlock;
   initBlock.addFunctions()
      (bind(registerRpcMethod, "update_notebook_exec_queue", updateExecQueue))
      (bind(registerRpcMethod, "execute_notebook_chunks", executeNotebookChunks));

   return initBlock.execute();
}

} // namespace notebook
} // namespace rmarkdown
} // namespace modules
} // namespace session
} // namespace rstudio