/*
 * quarkApp.cpp
 *
 *  Created on: Jun 3, 2017
 *      Author: ondra
 */

#include "quarkApp.h"

#include <couchit/changes.h>

#include <couchit/document.h>
#include <couchit/query.h>
#include <couchit/changeset.h>
#include <couchit/couchDB.h>

#include "error.h"


#include "init.h"
#include "logfile.h"
#include "marginTradingSvc.h"
#include "mockupmoneyserver.h"
#include "moneyServerClient.h"
#include "orderRangeError.h"
#include "tradeHelpers.h"
#include "views.h"

namespace quark {

const StrViewA QuarkApp::marketConfigDocName("settings");

QuarkApp::QuarkApp() {

}

void QuarkApp::processPendingOrders(Value user) {
	Value cmd = pendingOrders.unlock(user);
	while (cmd.defined()) {
		if (!processOrder2(cmd)) break;
		cmd = pendingOrders.unlock(user);
	}
}

bool QuarkApp::runOrder(Document order, bool update) {

	try {
		PQuarkApp me(this);
		Value user = order[OrderFields::user];

		//calculate budget
		auto b = calculateBudget(order);

		LOGDEBUG3("Budget calculated for the order", order.getIDValue(), b.toJson());

		if (!moneyService->allocBudget(order[OrderFields::user], order.getIDValue(), b,
							[me,order,update,user](IMoneySrvClient::AllocResult response) {
				QuarkApp *mptr = me;
				me->dispatcher.push([mptr,response,order,update,user]{
					//in positive response
					switch (response) {
					case IMoneySrvClient::allocOk:
						mptr->runOrder2(order, update);
						break;
					case IMoneySrvClient::allocReject:
						mptr->rejectOrderBudget(order, update);
						break;
					case IMoneySrvClient::allocError:
						mptr->rejectOrder(order,OrderErrorException(order.getIDValue(),
								OrderErrorException::internalError,
								"Failed to allocate budget (money server error)"),update);
						break;
					case IMoneySrvClient::allocTryAgain:
						mptr->runOrder(order,update);
					}


					mptr->processPendingOrders(user);
				});
				}
			)) return false;
		else {
			runOrder2(order,update);
			return true;
		}
	} catch (std::exception &e) {
		rejectOrder(order,
				OrderErrorException(order.getIDValue(),OrderErrorException::internalError,
						e.what()),false);
	}

}

void QuarkApp::freeBudget(const Document& order) {
	moneyService->allocBudget(order[OrderFields::user],
			order[OrderFields::orderId], OrderBudget(), nullptr);
}

void QuarkApp::rejectOrder(Document order, const OrderErrorException &e, bool update) {
	LOGDEBUG4("Order rejected", order.getIDValue(), e.what(), update?"update":"new order");
	const OrderRangeError *re = dynamic_cast<const OrderRangeError *>(&e);
	Value rangeValue;
	if (re) {
		rangeValue = re->getRangeValue();
	}

	try {

		if (update) {
			order(OrderFields::updateStatus,Status::strRejected)
			     (OrderFields::updateReq,json::undefined);
		} else {
			order(OrderFields::status,Status::strRejected)
				 (OrderFields::finished,true);
		}
		order(OrderFields::error, Object("code",e.getCode())
								("message",e.getMessage())
								("rangeValue",rangeValue));
		ordersDb->put(order);
	} catch (UpdateException &e) {
		if (e.getErrors()[0].isConflict()) {
			logInfo({"Order was not rejected because has been changed", e.getErrors()[0].document});
			return;
			}
		throw;
	}
	if (!update) {
		freeBudget(order);
	}
}

void QuarkApp::runOrder2(Document order, bool update) {


	LOGDEBUG3("Executing order", order.getIDValue(), update?"(update)":"(new order)");
	//stage 2
	try {
		//create transaction item
		TxItem txi;
		txi.action = update?actionUpdateOrder:actionAddOrder;
		txi.orderId = order.getIDValue();
		txi.order = docOrder2POrder(order);


		if (order[OrderFields::status].getString() != Status::strActive) {
			try {
				Value limPrice = order[OrderFields::limitPrice];
				Value stopPrice = order[OrderFields::stopPrice];
				Value distance = order[OrderFields::trailingDistance];
				Value size = order[OrderFields::size];
				Value orgSize = order[OrderFields::origSize];

				//normalize values on order
				if (limPrice.defined())
					order.set(OrderFields::limitPrice, marketCfg->adjustPrice(limPrice.getNumber()));
				if (stopPrice.defined())
					order.set(OrderFields::stopPrice, marketCfg->adjustPrice(stopPrice.getNumber()));
				if (distance.defined())
					order.set(OrderFields::trailingDistance, marketCfg->adjustPrice(distance.getNumber()));
				if (size.defined())
					order.set(OrderFields::size, marketCfg->adjustSize(size.getNumber()));
				if (orgSize.defined())
					order.set(OrderFields::origSize, marketCfg->adjustSize(orgSize.getNumber()));

				order.set(OrderFields::status, Status::strActive);

				ordersDb->put(order);
			} catch (const UpdateException &e) {
				//in case of conflict...
				if (e.getErrors()[0].isConflict()) {
					logWarn({"Order creation conflict, waiting for fresh data", order.getIDValue()});
					return;
				}
				throw;
			}
		}

		//run transaction
		runTransaction(txi);


		//handle various exceptions
	 }catch (OrderRangeError &e) {
		 rejectOrder(order, e, update);
	 }

}


void QuarkApp::processOrder(Value cmd) {


	Value user = cmd[OrderFields::user];
	if (!pendingOrders.lock(user, cmd)) {
		LOGDEBUG3("Order delayed because user is locked", user, cmd[OrderFields::orderId]);
		return;
	}

	if (processOrder2(cmd))
		processPendingOrders(user);

};

bool QuarkApp::isUpdated(const Document &order) {
	return order[OrderFields::updateReq].defined();

}
bool QuarkApp::isCanceled(const Document &order) {
	return order[OrderFields::cancelReq].getBool();
}


bool QuarkApp::processOrder2(Value cmd) {
	Document order(cmd);

	LOGDEBUG2("Pop order", order.getIDValue());

	if (marketCfg == nullptr) {
		LOGDEBUG2("Rejected order (no market)", order.getIDValue());
		order(OrderFields::status, Status::strRejected)
		   (OrderFields::error,Object("message","market is not opened yet"))
		   (OrderFields::finished,true);
		ordersDb->put(order);
		return true;
	}

	//skip finished orders
	if (order[OrderFields::finished].getBool()) {
		LOGDEBUG2("Skipped finished order", order.getIDValue());
		return true;
	}

	if (isCanceled(order)) {
		cancelOrder(order);
		return true;
	}
	if (isUpdated(order)) {
		return updateOrder(order);
	}
	if (coreState.isKnownOrder(order.getIDValue())) {
		return true;
	}
	return runOrder(order, false);
}
void QuarkApp::cancelOrder(Document order) {
	Value orderId = order.getIDValue();
	bool isKnown = coreState.isKnownOrder(orderId);
	//request to cancel order
	LOGDEBUG2("Order is user canceled", orderId);

	order.unset(OrderFields::cancelReq);
	order.set(OrderFields::status, Status::strCanceled);
	order.set(OrderFields::finished, true);

	try {
		ordersDb->put(order);
	} catch (UpdateException &e) {
		if (e.getErrors()[0].isConflict()) {
			logInfo({"Order was not canceled because it changed",e.getErrors()[0].document});
		}
		return;
	}


	if (isKnown) {
		//remove order from market
		TxItem txi;
		txi.action = actionRemoveOrder;
		txi.orderId = orderId;
		runTransaction(txi);
	}
	//unblocks the blocked budget by this command
	//this can be done asynchronously without waiting
	freeBudget(order);
}

bool QuarkApp::updateOrder(Document order) {
	Value orderId = order.getIDValue();
	bool isKnown = coreState.isKnownOrder(orderId);
	Value req = order[OrderFields::updateReq];

	LOGDEBUG3("Order is updated by user", orderId, req);

	for (Value v : req) {
		order.set(v);
	}
	order.set(OrderFields::updateReq,json::undefined);
	order.set(OrderFields::updateStatus,"accepted");

	try {
		ordersDb->put(order);
	} catch (const UpdateException &e) {
		if (e.getErrors()[0].isConflict()) {
			logWarn({"Order update conflict, waiting for fresh data", order.getIDValue()});
			return true;
		}
		throw;
	}

	return runOrder(order, isKnown);
}


/*Document QuarkApp::saveOrder(Document order, Object newItems) {

		LOGDEBUG3("Save order state", order.getIDValue(), newItems);

		newItems.setBaseObject(order);

		try {
			Value v(newItems);
			logDebug(v);
			Document d(v);
			ordersDb->put(d);
			return d;
		} catch (UpdateException &e) {
			if (e.getError(0).isConflict()) {
				LOGDEBUG2("Save order conflict", order.getIDValue());
				order = ordersDb->get(order.getID());
				saveOrder(order,newItems);
			}
		}

}

*/

OrderBudget QuarkApp::calculateBudget(const Document &order) {

	if (order[OrderFields::finished].getBool()) return OrderBudget();

	OrderDir::Type dir = OrderDir::str[order[OrderFields::dir].getString()];
	double size = marketCfg->adjustSize(order[OrderFields::size].getNumber());

	double slippage = 1+ marketCfg->maxSlippagePtc/100.0;
	OrderContext::Type context = OrderContext::str[order[OrderFields::context].getString()];
	if (dir == OrderDir::sell && context != OrderContext::margin) {
		return OrderBudget(size,0);
	} else {

		double reqbudget;
		Value budget = order[OrderFields::budget];
		if (budget.defined()) {
			reqbudget = budget.getNumber();
		} else {
			Value limPrice = order[OrderFields::limitPrice];
			Value stopPrice = order[OrderFields::stopPrice];
			if (limPrice.defined()) {
				reqbudget = size*marketCfg->adjustPrice(limPrice.getNumber());
			} else if (stopPrice.defined()) {
				reqbudget = size*marketCfg->adjustPrice(stopPrice.getNumber()*slippage);
			} else {
				reqbudget = lastPrice * size * slippage;
			}

		}

		if (context == OrderContext::margin) {
			if (dir == OrderDir::sell)
				return OrderBudget(0,reqbudget,0,size).adjust(*marketCfg);
			else
				return OrderBudget(reqbudget,0,size,0).adjust(*marketCfg);
		} else {
			return OrderBudget(0, reqbudget).adjust(*marketCfg);
		}
	}

}



void QuarkApp::exitApp() {
	dispatcher.push(nullptr);
}


void QuarkApp::runTransaction(const TxItem& txitm) {
	transactionCounter++;
	OrdersToUpdate &o2u = o2u_1;
	tradeList.clear();
	o2u.clear();
	Changeset chset = tradesDb->createChangeset();
	try {
		coreState.matching(transactionCounter,Transaction(&txitm,1), [&](const ITradeResult &res){
			receiveResults(res,o2u,tradeList);
		});
	} catch (OrderErrorException &e) {
		Value order = ordersDb->get(e.getOrderId().getString(), CouchDB::flgNullIfMissing);
		if (order.isNull()) {
			logError({"Unknown order exception", e.getOrderId(), e.getCode(), e.getMessage()});
		} else {
			Document doc(order);
			rejectOrder(order,e,false);
		}
		return;
	}

	//commit all trades into DB
	//build list of trades to commit
	for (auto && v: tradeList) {
		chset.update(v);
	}
	//commit in one call (any possible conflicts are thrown here before orders are
	//sent to the money server
	chset.commit();
	Array keys;

	if (!tradeList.empty())
	{
		//now we need to send changes to money server(s)
		//first get all orders appears in all trades
		//some orders can appear multiple times
		//we can use not-yet updated version, because we only need some
		//attibutes, such a context or user,

		Query q = ordersDb->createQuery(View::includeDocs);

		keys.reserve(o2u.size());
		//prepare list of keys
		for (auto && v : tradeList) {
			//put orderIds as keys
			keys.push_back(v["buyOrder"]);
			keys.push_back(v["sellOrder"]);
		}
		//now create ordered list and remove duplicates, and setup the query
		Value k = Value(keys).sort(&Value::compare).uniq();
		q.keys(k);
		//execute query
		Result res = q.exec();
		//result must have exact same values as keys
		if (res.size() != k.size())
			throw std::runtime_error("Some trades refer to no-existing order - sanity check");

		//result is also ordered. We need to build ordering function
		//cmpRes allows to ask with orderId even if orders are objects.
		//orderId is eqaul to order with given id
		auto cmpRes = [](const Value &a, const Value &b) {
			if (a.type() == json::object) {
				if (b.type() == json::object)
					return Value::compare(a["id"],b["id"]) < 0;
				else
					return Value::compare(a["id"],b) < 0;
			} else {
				return Value::compare(a,b["id"]) < 0;
			}
		};

		//sends reports to money server(s)
		for (auto && v : tradeList) {
			IMoneySrvClient::TradeData td;
			IMoneySrvClient::BalanceChange bch;
			//first report trade
			extractTrade(v, td);
			moneySrvClient->reportTrade(lastTradeId, td); //TODO: check return value!
			//get order for buyOrder and sellOrder
			Value buyOrderId = v["buyOrder"];
			Value sellOrderId = v["sellOrder"];
			//we don't need to check, whether order were found, above sanity check is enough
			Value buyOrder = (*std::lower_bound(res.begin(), res.end(),buyOrderId, cmpRes))["doc"];
			Value sellOrder = (*std::lower_bound(res.begin(), res.end(),sellOrderId, cmpRes))["doc"];
			LOGDEBUG2("buyOrder", buyOrder);
			LOGDEBUG2("sellOrder", sellOrder);
			//extract balance change (and calculate fees)
			extractBalanceChange(buyOrder,v,bch,OrderDir::buy,*marketCfg);
			moneySrvClient->reportBalanceChange(bch);
			extractBalanceChange(sellOrder,v,bch,OrderDir::sell,*marketCfg);
			moneySrvClient->reportBalanceChange(bch);
			//commit trade
			moneySrvClient->commitTrade(td.id);
			//update last tradeId
			lastTradeId = td.id;
			//update last price
			lastPrice = td.price;
		}

	}
	//update all affected orders
	keys.clear();
	if (!o2u.empty())
	{
		bool rep;

		//prepare query
		Query q = ordersDb->createQuery(View::includeDocs);
		//load all keys to the query
		for (auto && x: o2u) {
					keys.push_back(x.first);
		}

		do {
			//query all changed orders
			q.keys(keys);
			Result res = q.exec();
			if (res.size() != keys.size())
				throw std::runtime_error("Matching engine refers some orders not found in the database - sanity check");
			//process results
			for (Row r : res) {

				//perform update
				Document d = o2u[r.key];
				LOGDEBUG3("Updating order (mass)", r.key, d);
				//the map contains only changes. Now set the base object onto the changes will be applied
				d.setBaseObject(r.doc);

				d.optimize();
				//calculate budget of final object
				auto budget = calculateBudget(d);

				//TODO check result of allocatio for some commands (trailing-limit)
				moneyService->allocBudget(d[OrderFields::user],d.getIDValue(),budget,nullptr);

				//put order for update
				chset.update(d);
			}
			try {
				//commit all order change
				chset.commit(*ordersDb);
				//in case of all success, we end here
				rep = false;
			} catch (UpdateException &e) {
				//there can be conflicts
				keys.clear();
				//because changes fron trading has highest priority
				//we have to reapply changes to new version of order
				for (auto err : e.getErrors()) {
					if (err.isConflict()) {
						//collect conflicts
						Value id = err.document["_id"];
						LOGDEBUG2("Update order conflict", id);
						o2u_2[id] = o2u_1[id];
						//create new query
						keys.push_back(id);
					} else {
						//other exceptions are not allowed here
						throw;
					}
				}
				//swap maps
				std::swap(o2u_1,o2u_2);
				//clear other map
				o2u_2.clear();
				//repeat
				rep = true;
			}
		}
		while (rep);
	}
	//everything done, let get next order

}


void QuarkApp::receiveResults(const ITradeResult& r, OrdersToUpdate &o2u, TradeList &trades) {
	time_t now;
	time(&now);
	switch (r.getType()) {
			case quark::trTrade: {
					const quark::TradeResultTrade &t = dynamic_cast<const quark::TradeResultTrade &>(r);
					double price = marketCfg->pipToPrice(t.getPrice());
					double amount = marketCfg->sizeToAmount(t.getSize());
					Value dir = OrderDir::str[t.getDir()];
					logInfo({"Trade",dir,price,amount});
					Document &buyOrder = o2u[t.getBuyOrder()->getId()];
					Document &sellOrder = o2u[t.getSellOrder()->getId()];
					if (t.isFullBuy())
						buyOrder(OrderFields::finished,true)
								(OrderFields::status,Status::strExecuted);
					 else
						buyOrder(OrderFields::size,marketCfg->sizeToAmount(t.getBuyOrder()->getSize()-t.getSize()));

					if (t.isFullSell())
						sellOrder(OrderFields::finished,true)
								(OrderFields::status,Status::strExecuted);
					else
						sellOrder(OrderFields::size,marketCfg->sizeToAmount(t.getSellOrder()->getSize()-t.getSize()));
					Document trade;


					trade("_id",createTradeId(t))
						 ("price",price)
						 ("buyOrder",t.getBuyOrder()->getId())
						 ("sellOrder",t.getSellOrder()->getId())
						 (OrderFields::size,amount)
						 ("dir",dir)
						 ("time",(std::size_t)now)
						 ("index",tradeCounter++);
					trade.optimize();
					trades.push_back(trade);
				}break;
			case quark::trOrderMove: {
					const quark::TradeResultOderMove &t = dynamic_cast<const quark::TradeResultOderMove &>(r);
					POrder o = t.getOrder();
					Document &changes = o2u[o->getId()];
					if (o->getLimitPrice()) changes(OrderFields::limitPrice,marketCfg->pipToPrice(o->getLimitPrice()));
					if (o->getTriggerPrice()) changes(OrderFields::stopPrice,marketCfg->pipToPrice(o->getTriggerPrice()));
				}break;
			case quark::trOrderOk: {
				}break;
			case quark::trOrderCancel: {
					const quark::TradeResultOrderCancel &t = dynamic_cast<const quark::TradeResultOrderCancel &>(r);
					Document &o = o2u[t.getOrder()->getId()];
					o(OrderFields::status,"canceled")
					 (OrderFields::finished,true)
					 ("cancel_code",t.getCode());
				}break;
			case quark::trOrderTrigger: {
					const quark::TradeResultOrderTrigger &t = dynamic_cast<const quark::TradeResultOrderTrigger &>(r);
					Document &o = o2u[t.getOrder()->getId()];
					OrderType::Type ty = t.getOrder()->getType();
					StrViewA st = OrderType::str[ty];
					o("type", st);
				}break;
			}
}

void QuarkApp::rejectOrderBudget(Document order, bool update) {

	rejectOrder(order,
			OrderErrorException(order.getRevValue(), OrderErrorException::insufficientFunds,"Insufficient Funds")
			,update);

}

POrder QuarkApp::docOrder2POrder(const Document& order) {
	POrder po;
	OrderJsonData odata;
	Value v;
	double x;

	OrderBudget b = calculateBudget(order);

	odata.id = order["_id"];
	odata.dir = String(order["dir"]);
	odata.type = String(order["type"]);
	x = order[OrderFields::size].getNumber();
	if (x < marketCfg->minSize)
		throw OrderRangeError(odata.id, OrderRangeError::minOrderSize,
				marketCfg->minSize);

	if (x > marketCfg->maxSize)
		throw OrderRangeError(odata.id, OrderRangeError::maxOrderSize,
				marketCfg->maxSize);

	odata.size = marketCfg->amountToSize(x);
	if ((v = order[OrderFields::limitPrice]).defined()) {
		x = v.getNumber();
		if (x < marketCfg->minPrice)
			throw OrderRangeError(odata.id, OrderRangeError::minPrice,
					marketCfg->minPrice);

		if (x > marketCfg->maxPrice)
			throw OrderRangeError(odata.id, OrderRangeError::maxPrice,
					marketCfg->maxPrice);

		odata.limitPrice = marketCfg->priceToPip(x);
	} else {
		odata.limitPrice = 0;
	}
	if ((v = order[OrderFields::stopPrice]).defined()) {
		x = v.getNumber();
		if (x < marketCfg->minPrice)
			throw OrderRangeError(odata.id, OrderRangeError::minPrice,
					marketCfg->minPrice);

		if (x > marketCfg->maxPrice)
			throw OrderRangeError(odata.id, OrderRangeError::maxPrice,
					marketCfg->maxPrice);

		odata.stopPrice = marketCfg->priceToPip(x);
	} else {
		odata.stopPrice = 0;
	}

	double totalBudget = b.marginLong+b.marginShort+b.currency;
	odata.budget = marketCfg->budgetToPip(totalBudget);
	odata.trailingDistance = marketCfg->priceToPip(
			order[OrderFields::trailingDistance].getNumber());
	odata.domPriority = order[OrderFields::domPriority].getInt();
	odata.queuePriority = order[OrderFields::queuePriority].getInt();
	po = new Order(odata);
	return po;
}


void QuarkApp::syncWithDb() {

	Value lastTrade = fetchLastTrade(*tradesDb);
	if (lastTrade != nullptr) {
		tradeCounter = lastTrade["index"].getUInt();
		lastPrice = lastTrade["price"].getNumber();
	} else {
		tradeCounter = 1;
		lastPrice = 1;
	}

}

void QuarkApp::mainloop() {


	ChangesFeed chfeed = ordersDb->createChangesFeed();
	exitFn = [&] {
		chfeed.cancelWait();
	};

	Value errorDoc = ordersDb->get(OrderFields::error, CouchDB::flgNullIfMissing);
	if (!errorDoc.isNull()) {
		logError("Error is signaled, engine stopped - please remove error file to continue");

		try {
			chfeed.setFilter(errorWait).since(ordersDb->getLastKnownSeqNumber()).setTimeout(-1)
					>> [](ChangedDoc chdoc) {
				if (chdoc.deleted) return false;
				return true;
			};
		} catch (CanceledException &e) {
			return;
		}

	}

	logInfo("==== Entering to main loop ====");

	auto loopBody = [&](ChangedDoc chdoc) {

		if (!chdoc.deleted) {
			dispatcher.push([=]{
				if (chdoc.id == marketConfigDocName) {
					try {
						marketCfg = new MarketConfig(chdoc.doc);
						initMoneyService();
						logInfo("Market configuration updated");

					} catch (std::exception &e) {
						logError( {	"MarketConfig update failed", e.what()});
					}
				} else if (chdoc.id.substr(0,8) != "_design/") {
					processOrder(chdoc.doc);
				}
			});
		}
		return true;
	};

	logInfo("==== Preload commands ====");

	Query q = ordersDb->createQuery(queueView);
	Result res = q.exec();
	for (Value v : res) {
		loopBody(v);
	}

	logInfo("==== Inside of main loop ====");


	try {
		chfeed.setFilter(queueView).since(res.getUpdateSeq()).setTimeout(-1)
				>> 	loopBody;

	} catch (CanceledException &e) {

	}

	logInfo("==== Leaving main loop ====");

	return;




}

void QuarkApp::initMoneyService() {

	Value cfg = marketCfg->moneyService;
	StrViewA type = cfg["type"].getString();
	PMoneySrvClient sv;
	if (type == "mockup") {
		Value maxBudget = cfg["maxBudget"];
		Value jlatency = cfg["latency"];
		if (!maxBudget.defined()) throw std::runtime_error("Missing 'maxBudget' in money service definition");
		if (!jlatency.defined()) throw std::runtime_error("Missing 'latency' in money service definition");
		OrderBudget b(maxBudget["asset"].getNumber(),maxBudget["currency"].getNumber()
				,maxBudget["marginLong"].getNumber(),maxBudget["marginShort"].getNumber(), 0 ,0);
		std::size_t latency =jlatency.getUInt();
		sv = new MockupMoneyService(b,latency);
	} else if (type == "singleJsonRPCServer"){
		Value addr = cfg["addr"];
		sv = new MoneyServerClient(
				new MoneySvcSupport(ordersDb, tradesDb,marketCfg,
						[&](Action a){this->dispatcher.push(a);}), addr.getString(), signature);
	} else {
		throw std::runtime_error("Unsupported money service");
	}
	moneySrvClient = new MarginTradingSvc(*positionsDb,marketCfg,sv);
	if (moneyService == nullptr) {
		moneyService = new MoneyService(moneySrvClient,marketCfg);
	} else {
		moneyService->setClient(moneySrvClient);
		moneyService->setMarketConfig(marketCfg);
	}
}


void QuarkApp::receiveMarketConfig() {
	Value doc = ordersDb->get(marketConfigDocName, CouchDB::flgNullIfMissing);
	if (doc != nullptr) {
		marketCfg = new MarketConfig(doc);
		initMoneyService();
	} else {
		throw std::runtime_error("No money service available");
	}
}


void QuarkApp::start(couchit::Config cfg, String signature)

{

	this->signature = signature;

	String dbprefix = cfg.databaseName;
	cfg.databaseName = dbprefix + "-orders";

	setUnhandledExceptionHandler([cfg]{

		//Puts error object to database to prevent engine restart
		//and exits through abort()

		String errdesc;


		try {

			try {
				throw;
			} catch(std::exception &e) {
				errdesc = e.what();
			} catch(...) {
				errdesc = "Undetermined exception - catch (...)";
			}

			CouchDB db(cfg);
			Document errdoc;
			errdoc.setID("error");
			errdoc("what",errdesc);
			time_t t;
			time(&t);
			errdoc("time",t);
			db.put(errdoc);
		} catch (std::exception &e) {
			logError({"Fatal error (...and database is not available)",errdesc,e.what()});
		}
		abort();

	});

	ordersDb = std::make_shared<CouchDB>(cfg);
	initOrdersDB(*ordersDb);

	cfg.databaseName = dbprefix + "-trades";
	tradesDb = std::make_shared<CouchDB>(cfg);
	initTradesDB(*tradesDb);

	cfg.databaseName = dbprefix + "-positions";
	positionsDb = std::make_shared<CouchDB>(cfg);
	initPositionsDB(*positionsDb);

/*	this->moneySrvClient = new MarginTradingSvc(*positionsDb,moneyService);
	this->moneyService = new MoneyService(this->moneySrvClient);
	receiveMarketConfig();*/
	syncWithDb();

	changesReader = std::thread([=]{
		try {
			mainloop();
		} catch (...) {
			unhandledException();
		}});


	try {
		Action a = dispatcher.pop();
		while (a != nullptr) {
			a();
			a = nullptr;
			a = dispatcher.pop();
		}
		exitFn();
		changesReader.join();
		moneyService = nullptr;
		moneySrvClient = nullptr;
	} catch (...) {
		unhandledException();
	}

}


bool QuarkApp::PendingOrders::lock(Value id, const Value& doc) {
	std::lock_guard<std::mutex> _(l);
	auto f = orders.find(id);
	if (f == orders.end()) {
		orders.operator [](id);
		return true;
	} else {
		orders[id].push(doc);
		return false;
	}
}

Value QuarkApp::PendingOrders::unlock(Value id) {
	std::lock_guard<std::mutex> _(l);
	auto f = orders.find(id);
	if (f == orders.end()) return undefined;
	if (f->second.empty()) {
		orders.erase(f);
		return undefined;
	}
	Value out = f->second.front();
	f->second.pop();
	return out;
}

String QuarkApp::createTradeId(const TradeResultTrade &tr) {
	//because one of orders are always fully executed, it should never generate trade again
	//so we can use its ID to build unique trade ID

	if (tr.isFullBuy()) {
		return String({"t.",tr.getBuyOrder()->getId().getString().substr(2)});
	}
	if (tr.isFullSell()) {
		return String({"t.",tr.getSellOrder()->getId().getString().substr(2)});
	}
	throw std::runtime_error("Reported partial matching for both orders, this should not happen");
}


void QuarkApp::PendingOrders::clear() {
	std::lock_guard<std::mutex> _(l);
	orders.clear();
}




} /* namespace quark */

