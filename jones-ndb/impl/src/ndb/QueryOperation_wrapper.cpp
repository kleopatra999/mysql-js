/*
 Copyright (c) 2015, Oracle and/or its affiliates. All rights
 reserved.
 
 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; version 2 of
 the License.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 02110-1301  USA
*/

#include "ndb_util/NdbQueryOperation.hpp"

#include "adapter_global.h"
#include "TransactionImpl.h"
#include "QueryOperation.h"

#include "node_buffer.h"

#include "JsWrapper.h"
#include "js_wrapper_macros.h"
#include "NativeMethodCall.h"
#include "NdbWrapperErrors.h"

using namespace v8;

#define SET_KEY(X,Y) X.Set(isolate, String::NewFromUtf8(isolate, Y))
#define GET_KEY(X) X.Get(isolate)

Eternal<String>    /* keys of NdbProjection */
  K_next,
  K_root,
  K_hasScan,
  K_keyFields,
  K_joinTo,
  K_depth,
  K_tableHandler,
  K_rowRecord,
  K_indexHandler,
  K_keyRecord,
  K_isPrimaryKey,
  K_relatedField,
  K_dbTable,
  K_dbIndex,
  K_level,
  K_data,
  K_tag;


V8WrapperFn queryPrepareAndExecute,
            querySetTransactionImpl,
            queryFetchAllResults,
            queryGetResult,
            queryClose;


class QueryOperationEnvelopeClass : public Envelope {
public:
  QueryOperationEnvelopeClass() : Envelope("QueryOperation") {
    addMethod("prepareAndExecute", queryPrepareAndExecute);
    addMethod("setTransactionImpl", querySetTransactionImpl);
    addMethod("fetchAllResults", queryFetchAllResults);
    addMethod("getResult", queryGetResult);
    addMethod("close", queryClose);
  }
};

QueryOperationEnvelopeClass QueryOperationEnvelope;

Local<Value> QueryOperation_Wrapper(QueryOperation *queryOp) {
  Local<Value> jsobj = QueryOperationEnvelope.wrap(queryOp);
  QueryOperationEnvelope.freeFromGC(queryOp, jsobj);
  return jsobj;
}


void setRowBuffers(QueryOperation *queryOp, Handle<Object> spec) {
  DEBUG_ENTER();
  Isolate * isolate = Isolate::GetCurrent();
  Record * record = 0;
  int level = spec->Get(GET_KEY(K_depth))->Int32Value();
  if(spec->Get(GET_KEY(K_rowRecord))->IsObject()) {
    record = unwrapPointer<Record *>(spec->Get(GET_KEY(K_rowRecord))->ToObject());
  }
  assert(record);
  queryOp->createRowBuffer(level, record);

  if(spec->Get(GET_KEY(K_relatedField))->IsNull()) {
    queryOp->levelIsJoinTable(level);
  }
}


const NdbQueryOperationDef * createTopLevelQuery(QueryOperation *queryOp,
                                                 Handle<Object> spec,
                                                 Handle<Object> keyBuffer) {
  DEBUG_ENTER();
  Isolate * isolate = Isolate::GetCurrent();
  NdbQueryBuilder *builder = queryOp->getBuilder();

  /* Pull values out of the JavaScript object */
  Local<Value> v;
  const Record * keyRecord = 0;
  const NdbDictionary::Table * table = 0;
  const NdbDictionary::Index * index = 0;

  v = spec->Get(GET_KEY(K_keyRecord));
  if(v->IsObject()) {
    keyRecord = unwrapPointer<const Record *>(v->ToObject());
  };
  v = spec->Get(GET_KEY(K_tableHandler));
  if(v->IsObject()) {
    v = v->ToObject()->Get(GET_KEY(K_dbTable));
    if(v->IsObject()) {
      table = unwrapPointer<const NdbDictionary::Table *>(v->ToObject());
    }
  }
  bool isPrimaryKey = spec->Get(GET_KEY(K_isPrimaryKey))->BooleanValue();
  const char * key_buffer = node::Buffer::Data(keyBuffer);
  if(! isPrimaryKey) {
    v = spec->Get(GET_KEY(K_indexHandler));
    if(v->IsObject()) {
      v = v->ToObject()->Get(GET_KEY(K_dbIndex));
      if(v->IsObject()) {
        index = unwrapPointer<const NdbDictionary::Index *> (v->ToObject());
      }
    }
    assert(index);
  }

  /* Build the key */
  int nKeyParts = keyRecord->getNoOfColumns();
  const NdbQueryOperand * key_parts[nKeyParts+1];

  for(int i = 0; i < nKeyParts ; i++) {
    size_t offset = keyRecord->getColumnOffset(i);
    size_t length = keyRecord->getValueLength(i, key_buffer + offset);
    offset += keyRecord->getValueOffset(i);  // accounts for length bytes
    key_parts[i] = builder->constValue(key_buffer + offset, length);
  }
  key_parts[nKeyParts] = 0;

  return queryOp->defineOperation(index, table, key_parts);
}

const NdbQueryOperationDef * createNextLevel(QueryOperation *queryOp,
                                             Handle<Object> spec,
                                             const NdbQueryOperationDef * parent) {
  NdbQueryBuilder *builder = queryOp->getBuilder();
  Isolate * isolate = Isolate::GetCurrent();

  /* Pull values out of the JavaScript object */
  Local<Value> v;
  const NdbDictionary::Table * table = 0;
  const NdbDictionary::Index * index = 0;
  int depth = spec->Get(GET_KEY(K_depth))->Int32Value();
  DEBUG_PRINT("Creating QueryOperationDef at level %d",depth);

  v = spec->Get(GET_KEY(K_tableHandler));
  if(v->IsObject()) {
    v = v->ToObject()->Get(GET_KEY(K_dbTable));
    if(v->IsObject()) {
      table = unwrapPointer<const NdbDictionary::Table *>(v->ToObject());
    }
  }
  bool isPrimaryKey = spec->Get(GET_KEY(K_isPrimaryKey))->BooleanValue();

  if(! isPrimaryKey) {
    v = spec->Get(GET_KEY(K_indexHandler));
    if(v->IsObject()) {
      v = v->ToObject()->Get(GET_KEY(K_dbIndex));
      if(v->IsObject()) {
        index = unwrapPointer<const NdbDictionary::Index *> (v->ToObject());
      }
    }
    assert(index);
  }

  v = spec->Get(GET_KEY(K_joinTo));
  Array * joinColumns = Array::Cast(*v);

  /* Build the key */
  int nKeyParts = joinColumns->Length();
  const NdbQueryOperand * key_parts[nKeyParts+1];

  for(int i = 0 ; i < nKeyParts ; i++) {
    String::Utf8Value column_name(joinColumns->Get(i));
    key_parts[i] = builder->linkedValue(parent, *column_name);
  }
  key_parts[nKeyParts] = 0;

  return queryOp->defineOperation(index, table, key_parts);
}

/* JS QueryOperation.create(ndbRootProjection, keyBuffer, depth)
*/
void createQueryOperation(const Arguments & args) {
  DEBUG_MARKER(UDEB_DEBUG);
  REQUIRE_ARGS_LENGTH(3);
  Isolate * isolate = Isolate::GetCurrent();

  int size = args[2]->Int32Value();
  QueryOperation * queryOperation = new QueryOperation(size);
  const NdbQueryOperationDef * root, * current;

  Local<Value> v;
  Local<Object> spec = args[0]->ToObject();

  setRowBuffers(queryOperation, spec);
  current = root = createTopLevelQuery(queryOperation, spec,
                                       args[1]->ToObject());

  while(! (v = spec->Get(GET_KEY(K_next)))->IsNull()) {
    spec = v->ToObject();
    current = createNextLevel(queryOperation, spec, current);
    assert(current->getOpNo() == spec->Get(GET_KEY(K_depth))->Uint32Value());
    setRowBuffers(queryOperation, spec);
  }
  queryOperation->prepare(root);
  args.GetReturnValue().Set(QueryOperation_Wrapper(queryOperation));
}

void querySetTransactionImpl(const Arguments &args) {
  REQUIRE_ARGS_LENGTH(1);

  typedef NativeVoidMethodCall_1_<QueryOperation, TransactionImpl *> MCALL;
  MCALL mcall(& QueryOperation::setTransactionImpl, args);
  mcall.run();
  
  args.GetReturnValue().SetUndefined();
}

// void prepareAndExecute() 
// ASYNC
void queryPrepareAndExecute(const Arguments &args) {
  EscapableHandleScope scope(args.GetIsolate());
  DEBUG_MARKER(UDEB_DEBUG);
  REQUIRE_ARGS_LENGTH(1);
  typedef NativeMethodCall_0_<int, QueryOperation> MCALL;
  MCALL * mcallptr = new MCALL(& QueryOperation::prepareAndExecute, args);
  mcallptr->errorHandler = getNdbErrorIfLessThanZero;
  mcallptr->runAsync();
  args.GetReturnValue().SetUndefined();
}

// fetchAllResults()
// ASYNC
void queryFetchAllResults(const Arguments &args) {
  EscapableHandleScope scope(args.GetIsolate());
  REQUIRE_ARGS_LENGTH(1);
  typedef NativeMethodCall_0_<int, QueryOperation> MCALL;
  MCALL * mcallptr = new MCALL(& QueryOperation::fetchAllResults, args);
  mcallptr->errorHandler = getNdbErrorIfLessThanZero;
  mcallptr->runAsync();
  args.GetReturnValue().SetUndefined();
}

void freeQueryResultAtGC(char *data, void *hint) {
  (void) hint;   // unused
  free(data);
}

void doNotFreeQueryResultAtGC(char *data, void *hint) {
  (void) hint;
  (void) data;
}

// getResult(id, objectWrapper):  IMMEDIATE
void queryGetResult(const Arguments & args) {
  REQUIRE_ARGS_LENGTH(2);
  v8::Isolate * isolate = args.GetIsolate();

  QueryOperation * op = unwrapPointer<QueryOperation *>(args.Holder());
  size_t id = args[0]->Uint32Value();
  Handle<Object> wrapper = args[1]->ToObject();

  QueryResultHeader * header = op->getResult(id);

  if(header) {
    if(header->data) {
      wrapper->Set(GET_KEY(K_data),
        node::Buffer::New(header->data,
                          op->getResultRowSize(header->depth),
                          doNotFreeQueryResultAtGC, 0));
    } else {
      wrapper->Set(GET_KEY(K_data), Null(isolate));
    }
    wrapper->Set(GET_KEY(K_level), v8::Uint32::New(isolate, header->depth));
    wrapper->Set(GET_KEY(K_tag),   v8::Uint32::New(isolate, header->tag));
    args.GetReturnValue().Set(true);
  } else {
    args.GetReturnValue().Set(false);
  }
}

// void close()
// ASYNC
void queryClose(const Arguments & args) {
  typedef NativeVoidMethodCall_0_<QueryOperation> NCALL;
  NCALL * ncallptr = new NCALL(& QueryOperation::close, args);
  ncallptr->runAsync();
  args.GetReturnValue().SetUndefined();
}

void QueryOperation_initOnLoad(Handle<Object> target) {
  Isolate * isolate = Isolate::GetCurrent();
  Local<Object> ibObj = Object::New(Isolate::GetCurrent());
  Local<String> ibKey = NEW_SYMBOL("QueryOperation");
  target->Set(ibKey, ibObj);

  DEFINE_JS_FUNCTION(ibObj, "create", createQueryOperation);

  SET_KEY(K_next, "next");
  SET_KEY(K_root, "root");
  SET_KEY(K_hasScan, "hasScan");
  SET_KEY(K_keyFields, "keyFields");
  SET_KEY(K_joinTo, "joinTo");
  SET_KEY(K_depth, "depth");
  SET_KEY(K_tableHandler, "tableHandler");
  SET_KEY(K_rowRecord, "rowRecord"),
  SET_KEY(K_indexHandler, "indexHandler");
  SET_KEY(K_keyRecord, "keyRecord");
  SET_KEY(K_isPrimaryKey, "isPrimaryKey");
  SET_KEY(K_relatedField, "relatedField");

  SET_KEY(K_dbTable, "dbTable");
  SET_KEY(K_dbIndex, "dbIndex");

  SET_KEY(K_level, "level");
  SET_KEY(K_data, "data");
  SET_KEY(K_tag, "tag");
}

