//
// Copyright (C) 2004-2008 Maciej Sobczak, Stephen Hutton
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//

#define SOCI_SOURCE
#include "soci/session.h"
#include "soci/connection-parameters.h"
#include "soci/connection-pool.h"
#include "soci/soci-backend.h"
#include "soci/query_transformation.h"
#include "soci/transaction.h"

using namespace soci;
using namespace soci::details;

namespace // anonymous
{

void ensureConnected(session_backend * backEnd)
{
    if (backEnd == NULL)
    {
        throw soci_error("Session is not connected.");
    }
}

// Standard logger class used by default.
class standard_logger_impl : public logger_impl
{
public:
    standard_logger_impl()
    {
        logStream_ = NULL;
    }

    virtual void start_query(std::string const & query)
    {
        if (logStream_ != NULL)
        {
            *logStream_ << query << '\n';
        }

        lastQuery_ = query;
    }

    virtual void set_stream(std::ostream * s)
    {
        logStream_ = s;
    }

    virtual std::ostream * get_stream() const
    {
        return logStream_;
    }

    virtual std::string get_last_query() const
    {
        return lastQuery_;
    }

private:
    virtual logger_impl* do_clone() const
    {
        return new standard_logger_impl;
    }

    std::ostream * logStream_;
    std::string lastQuery_;
};

} // namespace anonymous

session::session()
    : once(this), prepare(this), query_transformation_(NULL),
      logger_(new standard_logger_impl),
      uppercaseColumnNames_(false), backEnd_(NULL),
      isFromPool_(false), pool_(NULL),
      internalTransaction_(NULL),externalTransaction_(NULL),
      allowMultipleTransaction_(true)
{
}

session::session(connection_parameters const & parameters)
    : once(this), prepare(this), query_transformation_(NULL),
      logger_(new standard_logger_impl),
      lastConnectParameters_(parameters),
      uppercaseColumnNames_(false), backEnd_(NULL),
      isFromPool_(false), pool_(NULL),
      internalTransaction_(NULL),externalTransaction_(NULL),
      allowMultipleTransaction_(true)
{
    open(lastConnectParameters_);
}

session::session(backend_factory const & factory,
    std::string const & connectString)
    : once(this), prepare(this), query_transformation_(NULL),
    logger_(new standard_logger_impl),
      lastConnectParameters_(factory, connectString),
      uppercaseColumnNames_(false), backEnd_(NULL),
      isFromPool_(false), pool_(NULL),
      internalTransaction_(NULL),externalTransaction_(NULL),
      allowMultipleTransaction_(true)
{
    open(lastConnectParameters_);
}

session::session(std::string const & backendName,
    std::string const & connectString)
    : once(this), prepare(this), query_transformation_(NULL),
      logger_(new standard_logger_impl),
      lastConnectParameters_(backendName, connectString),
      uppercaseColumnNames_(false), backEnd_(NULL),
      isFromPool_(false), pool_(NULL),
      internalTransaction_(NULL),externalTransaction_(NULL),
      allowMultipleTransaction_(true)
{
    open(lastConnectParameters_);
}

session::session(std::string const & connectString)
    : once(this), prepare(this), query_transformation_(NULL),
      logger_(new standard_logger_impl),
      lastConnectParameters_(connectString),
      uppercaseColumnNames_(false), backEnd_(NULL),
      isFromPool_(false), pool_(NULL),
      internalTransaction_(NULL),externalTransaction_(NULL),
      allowMultipleTransaction_(true)
{
    open(lastConnectParameters_);
}

session::session(connection_pool & pool)
    : query_transformation_(NULL),
      logger_(new standard_logger_impl),
      isFromPool_(true), pool_(&pool),
      internalTransaction_(NULL),externalTransaction_(NULL),
      allowMultipleTransaction_(true)
{
    poolPosition_ = pool.lease();
    session & pooledSession = pool.at(poolPosition_);

    once.set_session(&pooledSession);
    prepare.set_session(&pooledSession);
    backEnd_ = pooledSession.get_backend();
}

session::~session()
{
    this->delete_internal_transaction();

    if (isFromPool_)
    {
        pool_->give_back(poolPosition_);
    }
    else
    {
        delete query_transformation_;
        delete backEnd_;
    }
}

void session::delete_internal_transaction()
{
    if (this->internalTransaction_ != NULL)
    {
        bool is_active = this->internalTransaction_->is_active();

        this->internalTransaction_->disabled();
        delete this->internalTransaction_;

        this->internalTransaction_ = NULL;

        if (is_active &&
            backEnd_)
        {
            backEnd_->rollback();
        }
    }
}

void session::open(connection_parameters const & parameters)
{
    if (isFromPool_)
    {
        session & pooledSession = pool_->at(poolPosition_);
        pooledSession.open(parameters);
        backEnd_ = pooledSession.get_backend();
    }
    else
    {
        if (backEnd_ != NULL)
        {
            throw soci_error("Cannot open already connected session.");
        }

        backend_factory const * const factory = parameters.get_factory();
        if (factory == NULL)
        {
            throw soci_error("Cannot connect without a valid backend.");
        }

        backEnd_ = factory->make_session(parameters);
        lastConnectParameters_ = parameters;
    }
}

void session::open(backend_factory const & factory,
    std::string const & connectString)
{
    open(connection_parameters(factory, connectString));
}

void session::open(std::string const & backendName,
    std::string const & connectString)
{
    open(connection_parameters(backendName, connectString));
}

void session::open(std::string const & connectString)
{
    open(connection_parameters(connectString));
}

void session::close()
{
    this->delete_internal_transaction();

    if (isFromPool_)
    {
        pool_->at(poolPosition_).close();
        backEnd_ = NULL;
    }
    else
    {
        delete backEnd_;
        backEnd_ = NULL;
    }
}

void session::reconnect()
{
    if (isFromPool_)
    {
        session & pooledSession = pool_->at(poolPosition_);
        pooledSession.reconnect();
        backEnd_ = pooledSession.get_backend();
    }
    else
    {
        backend_factory const * const lastFactory = lastConnectParameters_.get_factory();
        if (lastFactory == NULL)
        {
            throw soci_error("Cannot reconnect without previous connection.");
        }

        if (backEnd_ != NULL)
        {
            close();
        }

        // Indicate that we're reconnecting using a special parameter which can
        // be used by some backends (currently only ODBC) that interactive
        // prompts should be suppressed, as they would be unexpected during
        // reconnection, which may happen automatically and not in the result
        // of a user action.
        connection_parameters reconnectParameters(lastConnectParameters_);
        reconnectParameters.set_option(option_reconnect, option_true);
        backEnd_ = lastFactory->make_session(reconnectParameters);
    }
}

bool session::is_connected() const SOCI_NOEXCEPT
{
    try
    {
        return backEnd_ && backEnd_->is_connected();
    }
    catch (...)
    {
        // We must not throw from here, so just handle any exception as an
        // indication that the database connection is not available any longer.
        return false;
    }
}

void session::allow_multiple_transaction( bool allow_multiple_transaction )
{
    this->allowMultipleTransaction_ = allow_multiple_transaction;
}

bool session::allow_multiple_transaction() const
{
    return this->allowMultipleTransaction_;
}

transaction * session::current_transaction() const
{
    if (this->internalTransaction_ == NULL ||
        this->internalTransaction_->is_active() == false)
    {
        return this->externalTransaction_;
    }
    else
        return this->internalTransaction_;
}

bool session::current_transaction_is_active() const
{
    return current_transaction() && current_transaction()->is_active(); //this->transaction_ != NULL ? transaction_->is_active(): false;
}

unsigned short session::t_isolation_level()
{
    ensureConnected(backEnd_);

    return backEnd_->t_isolation_level();
}

bool session::t_isolation_level( unsigned short level )
{
    ensureConnected(backEnd_);

    return backEnd_->t_isolation_level( level );
}

transaction * session::begin()
{
    ensureConnected(backEnd_);

    bool is_active = false;
    transaction * transactionToUse = NULL;

    if (this->externalTransaction_ != NULL) //External transaction object associated?
    {
        is_active = this->externalTransaction_->is_active();  //Is active?
        this->externalTransaction_->active();
        // this->externalTransaction_->handled_ = false;   //Reactive the transaction
        // this->externalTransaction_->status_  = 1;       //Active

        transactionToUse = this->externalTransaction_; //Use the external transaction
    }
    else if (this->internalTransaction_ == NULL) //No transaction object associated yet?
    {
        //Create transaction object because not associated yet
        //Mark the by_session to true, to indicate is create inside of session object.
        //by_session help to know when is need delete the pointer and prevent memory leak
        this->internalTransaction_ = new transaction(*this, true); //No auto start transaction private constructor. Because we start the transaction in the next line

        transactionToUse = this->internalTransaction_; //Use internal created transaction
    }
    else
    {
        is_active = this->internalTransaction_->is_active(); //Is active?
        this->internalTransaction_->active();
        // this->internalTransaction_->handled_ = false;    //Reactive the transaction
        // this->internalTransaction_->status_  = 1;        //Active

        transactionToUse = this->internalTransaction_;   //Use internal already created transaction
    }

    if (is_active == false) //Only if needed call to begin
    {
        backEnd_->begin();
    }

    return transactionToUse;
}

void session::begin_external_transaction()
{
    if (this->internalTransaction_ != NULL)
    {
        bool is_active = this->internalTransaction_->is_active();

        this->internalTransaction_->disabled();
        // this->internalTransaction_->handled_ = true;
        // this->internalTransaction_->status_  = 0; //Disabled

        if (is_active) //Only if needed call to rollback
        {
            backEnd_->rollback();
        }
    }

    ensureConnected(backEnd_);

    backEnd_->begin();
}

void session::commit()
{
    ensureConnected(backEnd_);

    backEnd_->commit();

    if (this->current_transaction() &&
        this->current_transaction()->by_session())
    {
        transaction * currentTransaction =  this->current_transaction();
        currentTransaction->commited();
        // currentTransaction->handled_ = true; //Disable the transaction object. Prevent any operation in transaction object.
        // currentTransaction->status_  = 2;    //Commited
    }
}

void session::rollback()
{
    ensureConnected(backEnd_);

    backEnd_->rollback();

    if (this->current_transaction() &&
        this->current_transaction()->by_session())
    {
        transaction * currentTransaction =  this->current_transaction();
        currentTransaction->rolledback();
        // currentTransaction->handled_ = true; //Disable the transaction object. Prevent any operation in transaction object.
        // currentTransaction->status_  = 3;    //Rolled back
    }
}

std::ostringstream & session::get_query_stream()
{
    if (isFromPool_)
    {
        return pool_->at(poolPosition_).get_query_stream();
    }
    else
    {
        return query_stream_;
    }
}

std::string session::get_query() const
{
    if (isFromPool_)
    {
        return pool_->at(poolPosition_).get_query();
    }
    else
    {
        // preserve logical constness of get_query,
        // stream used as read-only here,
        session* pthis = const_cast<session*>(this);

        // sole place where any user-defined query transformation is applied
        if (query_transformation_)
        {
            return (*query_transformation_)(pthis->get_query_stream().str());
        }
        return pthis->get_query_stream().str();
    }
}


void session::set_query_transformation_(cxx_details::auto_ptr<details::query_transformation_function>& qtf)
{
    if (isFromPool_)
    {
        pool_->at(poolPosition_).set_query_transformation_(qtf);
    }
    else
    {
        delete query_transformation_;
        query_transformation_= qtf.release();
    }
}

void session::set_logger(logger const & logger)
{
    if (isFromPool_)
    {
        pool_->at(poolPosition_).set_logger(logger);
    }
    else
    {
        logger_ = logger;
    }
}

logger const & session::get_logger() const
{
    if (isFromPool_)
    {
        return pool_->at(poolPosition_).get_logger();
    }
    else
    {
        return logger_;
    }
}

void session::set_log_stream(std::ostream * s)
{
    if (isFromPool_)
    {
        pool_->at(poolPosition_).set_log_stream(s);
    }
    else
    {
        logger_.set_stream(s);
    }
}

std::ostream * session::get_log_stream() const
{
    if (isFromPool_)
    {
        return pool_->at(poolPosition_).get_log_stream();
    }
    else
    {
        return logger_.get_stream();
    }
}

void session::log_query(std::string const & query)
{
    if (isFromPool_)
    {
        pool_->at(poolPosition_).log_query(query);
    }
    else
    {
        logger_.start_query(query);
    }
}

std::string session::get_last_query() const
{
    if (isFromPool_)
    {
        return pool_->at(poolPosition_).get_last_query();
    }
    else
    {
        return logger_.get_last_query();
    }
}

void session::set_got_data(bool gotData)
{
    if (isFromPool_)
    {
        pool_->at(poolPosition_).set_got_data(gotData);
    }
    else
    {
        gotData_ = gotData;
    }
}

bool session::got_data() const
{
    if (isFromPool_)
    {
        return pool_->at(poolPosition_).got_data();
    }
    else
    {
        return gotData_;
    }
}

void session::uppercase_column_names(bool forceToUpper)
{
    if (isFromPool_)
    {
        pool_->at(poolPosition_).uppercase_column_names(forceToUpper);
    }
    else
    {
        uppercaseColumnNames_ = forceToUpper;
    }
}

bool session::get_uppercase_column_names() const
{
    if (isFromPool_)
    {
        return pool_->at(poolPosition_).get_uppercase_column_names();
    }
    else
    {
        return uppercaseColumnNames_;
    }
}

bool session::get_next_sequence_value(std::string const & sequence, long long & value)
{
    ensureConnected(backEnd_);

    return backEnd_->get_next_sequence_value(*this, sequence, value);
}

bool session::get_last_insert_id(std::string const & sequence, long long & value)
{
    ensureConnected(backEnd_);

    return backEnd_->get_last_insert_id(*this, sequence, value);
}

details::once_temp_type session::get_table_names()
{
    ensureConnected(backEnd_);

    return once << backEnd_->get_table_names_query();
}

details::prepare_temp_type session::prepare_table_names()
{
    ensureConnected(backEnd_);

    return prepare << backEnd_->get_table_names_query();
}

details::prepare_temp_type session::prepare_column_descriptions(std::string & table_name)
{
    ensureConnected(backEnd_);

    return prepare << backEnd_->get_column_descriptions_query(), use(table_name, "t");
}

ddl_type session::create_table(const std::string & tableName)
{
    ddl_type ddl(*this);

    ddl.create_table(tableName);
    ddl.set_tail(")");

    return ddl;
}

void session::drop_table(const std::string & tableName)
{
    ensureConnected(backEnd_);

    once << backEnd_->drop_table(tableName);
}

void session::truncate_table(const std::string & tableName)
{
    ensureConnected(backEnd_);

    once << backEnd_->truncate_table(tableName);
}

ddl_type session::add_column(const std::string & tableName,
    const std::string & columnName, data_type dt,
    int precision, int scale)
{
    ddl_type ddl(*this);

    ddl.add_column(tableName, columnName, dt, precision, scale);

    return ddl;
}

ddl_type session::alter_column(const std::string & tableName,
    const std::string & columnName, data_type dt,
    int precision, int scale)
{
    ddl_type ddl(*this);

    ddl.alter_column(tableName, columnName, dt, precision, scale);

    return ddl;
}

ddl_type session::drop_column(const std::string & tableName,
    const std::string & columnName)
{
    ddl_type ddl(*this);

    ddl.drop_column(tableName, columnName);

    return ddl;
}

std::string session::empty_blob()
{
    ensureConnected(backEnd_);

    return backEnd_->empty_blob();
}

std::string session::nvl()
{
    ensureConnected(backEnd_);

    return backEnd_->nvl();
}

std::string session::get_dummy_from_table() const
{
    ensureConnected(backEnd_);

    return backEnd_->get_dummy_from_table();
}

std::string session::get_dummy_from_clause() const
{
    std::string clause = get_dummy_from_table();
    if (!clause.empty())
        clause.insert(0, " from ");

    return clause;
}

void session::set_failover_callback(failover_callback & callback)
{
    ensureConnected(backEnd_);

    backEnd_->set_failover_callback(callback, *this);
}

std::string session::get_backend_name() const
{
    ensureConnected(backEnd_);

    return backEnd_->get_backend_name();
}

statement_backend * session::make_statement_backend()
{
    ensureConnected(backEnd_);

    return backEnd_->make_statement_backend();
}

rowid_backend * session::make_rowid_backend()
{
    ensureConnected(backEnd_);

    return backEnd_->make_rowid_backend();
}

blob_backend * session::make_blob_backend()
{
    ensureConnected(backEnd_);

    return backEnd_->make_blob_backend();
}
