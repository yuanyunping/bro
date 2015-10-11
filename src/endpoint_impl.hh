#ifndef BROKER_ENDPOINT_IMPL_HH
#define BROKER_ENDPOINT_IMPL_HH

#include "broker/endpoint.hh"
#include "broker/report.hh"
#include "broker/store/identifier.hh"
#include "broker/store/query.hh"
#include "subscription.hh"
#include "peering_impl.hh"
#include "util/radix_tree.hh"
#include "atoms.hh"
#include <caf/actor.hpp>
#include <caf/spawn.hpp>
#include <caf/send.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/scoped_actor.hpp>
#include <caf/io/remote_actor.hpp>
#include <unordered_set>
#include <sstream>
#include <assert.h>


#ifdef DEBUG
// So that we don't have a recursive expansion from sending messages via the
// report::manager endpoint.
#define BROKER_ENDPOINT_DEBUG(endpoint_pointer, subtopic, msg) \
	if ( endpoint_pointer != broker::report::manager ) \
		broker::report::send(broker::report::level::debug, subtopic, msg)
#else
#define BROKER_ENDPOINT_DEBUG(endpoint_pointer, subtopic, msg)
#endif

namespace broker {

static std::string to_string(const topic_set& ts)
	{
	std::string rval{"{"};

	bool first = true;

	for ( const auto& e : ts )
		{
		if ( first )
			first = false;
		else
			rval += ", ";

		rval += e.first;
		}

	rval += "}";
	return rval;
	}

static void ocs_update(const caf::actor& q, peering::impl pi,
                       outgoing_connection_status::tag t, std::string name = "")
	{
	peering p{std::unique_ptr<peering::impl>(new peering::impl(std::move(pi)))};
	caf::anon_send(q, outgoing_connection_status{std::move(p), t,
	                                             std::move(name)});
	}

static void ics_update(const caf::actor& q, std::string name,
                       incoming_connection_status::tag t)
	{ caf::anon_send(q, incoming_connection_status{t, std::move(name)}); }

class endpoint_actor : public caf::event_based_actor {

public:

	endpoint_actor(const endpoint* ep, std::string arg_name, int flags,
	               caf::actor ocs_queue, caf::actor ics_queue)
		: name(std::move(arg_name)), behavior_flags(flags)
		{
		using namespace caf;
		using namespace std;
		auto ocs_established = outgoing_connection_status::tag::established;
		auto ocs_disconnect = outgoing_connection_status::tag::disconnected;
		auto ocs_incompat = outgoing_connection_status::tag::incompatible;

		active = {
		[=](int version)
			{
			return make_message(BROKER_PROTOCOL_VERSION == version,
			                    BROKER_PROTOCOL_VERSION);
			},
		[=](peer_atom, actor& p, peering::impl& pi)
			{
			auto it = peers.find(p.address());

			if ( it != peers.end() )
				{
				ocs_update(ocs_queue, move(pi), ocs_established,
		                   it->second.name);
				return;
				}

			sync_send(p, BROKER_PROTOCOL_VERSION).then(
				[=](const sync_exited_msg& m)
					{
					ocs_update(ocs_queue, move(pi), ocs_disconnect);
					},
				[=](bool compat, int their_version)
					{
					if ( ! compat )
						ocs_update(ocs_queue, move(pi), ocs_incompat);
					else
						{
						topic_set subscr = get_all_subscriptions();
						std::cout << name << " initiate peering with " << get_peer_name(p) << std::endl;

						//sync_send(p, peer_atom::value, this, name, advertised_subscriptions).then(
						sync_send(p, peer_atom::value, this, name, subscr, sub_topic_actor).then(
							[=](const sync_exited_msg& m)
								{
								ocs_update(ocs_queue, move(pi), ocs_disconnect);
								},
							[=](string& pname, topic_set& ts, topic_actor_set& sub_id_list)
								{
                add_peer(move(p), pname, move(ts), false, sub_id_list);
								ocs_update(ocs_queue, move(pi), ocs_established,
								           move(pname));
								}
						);
						}
					},
				others() >> [=]
					{
					ocs_update(ocs_queue, move(pi), ocs_incompat);
					}
			);
			},
		[=](peer_atom, actor& p, string& pname, topic_set& ts, topic_actor_set & sub_id_list)
			{
			std::cout <<  name  << " received peer_atom: " << to_string(ts) 
								<<  ", current_sender " << get_peer_name(current_sender()) << ", " 
								<< caf::to_string(current_message()) << std::endl;

			ics_update(ics_queue, pname,
			           incoming_connection_status::tag::established);

			// Propagate the own subscriptions + the ones of all other neighbors
			topic_set subscr = get_all_subscriptions();

			for(auto& t: ts)
				publish_subscription_operation(t.first, p, sub_atom::value, p.address());

			add_peer(move(p), move(pname), move(ts), true, sub_id_list);

			return make_message(name, subscr, sub_topic_actor);
			},
		[=](unpeer_atom, const actor& p)
			{
			auto itp = peers.find(p.address());

			if ( itp == peers.end() )
			    return;

			BROKER_DEBUG("endpoint." + name,
			             "Unpeered with: '" + itp->second.name + "'");

			std::cout << name << " unpeered with " << itp->second.name << std::endl; 

			if ( itp->second.incoming )
				ics_update(ics_queue, itp->second.name,
				           incoming_connection_status::tag::disconnected);

			// update routing information after p left
			update_routing_information(p);

			demonitor(p);
			peers.erase(itp);
			peer_subscriptions.erase(p.address());
			},
		[=](const down_msg& d)
			{
			demonitor(d.source);

			auto itp = peers.find(d.source);

			if ( itp != peers.end() )
				{
				BROKER_DEBUG("endpoint." + name,
				             "Peer down: '" + itp->second.name + "'");

				if ( itp->second.incoming )
					ics_update(ics_queue, itp->second.name,
					           incoming_connection_status::tag::disconnected);

				peers.erase(itp);
				peer_subscriptions.erase(d.source);
				return;
				}

			auto s = local_subscriptions.erase(d.source);

			if ( ! s )
				return;

			BROKER_DEBUG("endpoint." + name,
			             "Local subscriber down with subscriptions: "
			             + to_string(s->subscriptions));

			for ( auto& sub : s->subscriptions )
				if ( ! local_subscriptions.have_subscriber_for(sub.first) )
					unadvertise_subscription(topic{move(sub.first)});
			},
		[=](unsub_atom, const topic& t, const actor& p, caf::actor_addr sub_id)
			{
			BROKER_DEBUG("endpoint." + name,
			             "Peer '" + get_peer_name(p) + "' unsubscribed to '"
			             + t + "'");
			std::cout << name  << " received unsubscribe msg for topic (" << t << ", " <<  caf::to_string(sub_id) << ") from actor "  << get_peer_name(p) << std::endl;

			// update routing information
			topic_actor_pair tp = make_pair(t, sub_id);
			update_routing_information_topic(p, tp);
			if(routing_info.find(p.address()) != routing_info.end())
				routing_info[p.address()].erase(tp);

			peer_subscriptions.unregister_topic(t, p.address());
			},
		[=](sub_atom, topic& t, actor& p, caf::actor_addr sub_id)
			{
			BROKER_DEBUG("endpoint." + name,
			             "Peer '" + get_peer_name(p) + "' subscribed to '"
			             + t + "'");
      std::cout << name <<  ", received sub from "  << get_peer_name(p) << " for " << t << std::endl;

			// check if subscription is present already for originating node
			topic_actor_pair tp = std::make_pair(t, sub_id);
			if(!peer_subscriptions.contains(p.address(), t) 
				&& sub_topic_actor.find(tp) == sub_topic_actor.end())
				{
				peer_subscriptions.register_topic(t, p);
				publish_subscription_operation(t, p, sub_atom::value, sub_id);
				sub_topic_actor.insert(tp);
				routing_info[p.address()].insert(tp);
				}

			sub_mapping[tp][p] = 42;
			},
		[=](master_atom, store::identifier& id, actor& a)
			{
			if ( local_subscriptions.exact_match(id) )
				{
				report::error("endpoint." + name + ".store.master." + id,
				              "Failed to register master data store with id '"
				              + id + "' because a master already exists with"
				                     " that id.");
				return;
				}

			BROKER_DEBUG("endpoint." + name,
			             "Attached master data store named '" + id + "'");
			attach(move(id), move(a));
			},
		[=](local_sub_atom, topic& t, actor& a)
			{
			BROKER_DEBUG("endpoint." + name,
			             "Attached local queue for topic '" + t + "'");
			attach(move(t), move(a));
			},
		[=](const topic& t, broker::message& msg, int flags)
			{
			// we are the initial sender
			if(!current_sender()) 
				{
				BROKER_ENDPOINT_DEBUG(ep, "endpoint." + name,
				                      "Publish local message with topic '" + t
				                      + "': " + to_string(msg));
				std::cout << name << " Publish local message with topic '" << t
				               << "': " << to_string(msg) << " and flags " << to_string(flags) << std::endl;
				publish_locally(t, msg, flags, false);
				}
			// we received the message from a neighbor
			else
				{
				BROKER_ENDPOINT_DEBUG(ep, "endpoint." + name,
				                      "Got remote message from peer '"
				                      + get_peer_name(current_sender())
				                      + "', topic '" + t + "': "
				                      + to_string(msg));
				std::cout<< name << " Got remote message from peer '"
				               << get_peer_name(current_sender()) << "', topic '" << t << "': "
	                     << to_string(msg) << " and flags " << to_string(flags) << std::endl;
				publish_locally(t, msg, flags, true);
				}

     	publish_current_msg_to_peers(t, flags);

    	},
		[=](store_actor_atom, const store::identifier& n)
			{
			return find_master(n);
			},
		[=](const store::identifier& n, const store::query& q,
		    const actor& requester)
			{
			auto master = find_master(n);

			if ( master )
				{
				BROKER_DEBUG("endpoint." + name, "Forwarded data store query: "
				             + caf::to_string(current_message()));
				forward_to(master);
				}
			else
				{
				BROKER_DEBUG("endpoint." + name,
				             "Failed to forward data store query: "
				             + caf::to_string(current_message()));
				send(requester, this,
				     store::result(store::result::status::failure));
				}
			},
		on<store::identifier, anything>() >> [=](const store::identifier& id)
			{
			// This message should be a store update operation.
			auto master = find_master(id);

			if ( master )
				{
				BROKER_DEBUG("endpoint." + name, "Forwarded data store update: "
				             + caf::to_string(current_message()));
				forward_to(master);
				}
			else
				report::warn("endpoint." + name + ".store.master." + id,
				             "Data store update dropped due to nonexistent "
				             " master with id '" + id + "'");
			},
		[=](flags_atom, int flags)
			{
			bool auto_before = (behavior_flags & AUTO_ADVERTISE);
			behavior_flags = flags;
			bool auto_after = (behavior_flags & AUTO_ADVERTISE);

			if ( auto_before == auto_after )
				return;

			if ( auto_before )
				{
				topic_set to_remove;

				for ( const auto& t : advertised_subscriptions )
					if ( advert_acls.find(t.first) == advert_acls.end() )
						to_remove.insert({t.first, true});

				BROKER_DEBUG("endpoint." + name, "Toggled AUTO_ADVERTISE off,"
				                                 " no longer advertising: "
				             + to_string(to_remove));

				for ( const auto& t : to_remove )
					unadvertise_subscription(topic{t.first});

				return;
				}

			BROKER_DEBUG("endpoint." + name, "Toggled AUTO_ADVERTISE on");

			for ( const auto& t : local_subscriptions.topics() )
				advertise_subscription(topic{t.first});
			},
		[=](acl_pub_atom, topic& t)
			{
			BROKER_DEBUG("endpoint." + name, "Allow publishing topic: " + t);
			pub_acls.insert({move(t), true});
			},
		[=](acl_unpub_atom, const topic& t)
			{
			BROKER_DEBUG("endpoint." + name, "Disallow publishing topic: " + t);
			pub_acls.erase(t);
			},
		[=](advert_atom, string& t)
			{
			BROKER_DEBUG("endpoint." + name,
			             "Allow advertising subscription: " + t);

			if ( advert_acls.insert({t, true}).second &&
			     local_subscriptions.exact_match(t) )
				// Now permitted to advertise an existing subscription.
				advertise_subscription(move(t));
			},
		[=](unadvert_atom, string& t)
			{
			BROKER_DEBUG("endpoint." + name,
			             "Disallow advertising subscription: " + t);

			if ( advert_acls.erase(t) && local_subscriptions.exact_match(t) )
				// No longer permitted to advertise an existing subscription.
				unadvertise_subscription(move(t));
			},
		others() >> [=]
			{
			report::warn("endpoint." + name, "Got unexpected message: "
			             + caf::to_string(current_message()));
			}
		};
		}

private:

	caf::behavior make_behavior() override
		{
		return active;
		}

	std::string get_peer_name(const caf::actor_addr& a) const
		{
		auto it = peers.find(a);

		if ( it == peers.end() )
			return "<unknown>";

		return it->second.name;
		}

	std::string get_peer_name(const caf::actor& p) const
		{ return get_peer_name(p.address()); }

	void add_peer(caf::actor p, std::string peer_name, topic_set ts,
								bool incoming, topic_actor_set peer_sub_id_list)
		{
		BROKER_DEBUG("endpoint." + name, "Peered with: '" + peer_name
								 + "', subscriptions: " + to_string(ts));

		std::cout << name  <<  " adding peer '"  <<  peer_name
											<<  "', subscriptions: "  << to_string(ts)  << std::endl;

		demonitor(p);
		monitor(p);

		peers[p.address()] = {p, peer_name, incoming};
		routing_info[p.address()] = peer_sub_id_list;

		/*std::cout << " sub_topic_actor: " << std::endl;
		for(auto&t: sub_topic_actor)
			std::cout  << "   - "  << t.first << ", " << caf::to_string(t.second) << std::endl;

		std::cout << " peer_sub_id_list: " << std::endl;
		for(auto&t: peer_sub_id_list)
			std::cout  << "   - "  << t.first << ", " << caf::to_string(t.second) << std::endl;*/

		// iterate over the topic knowledge of new peer
		for(auto& tp: peer_sub_id_list)
			{
				if(sub_topic_actor.find(tp) == sub_topic_actor.end())	
					{
					sub_topic_actor.insert(tp);
					peer_subscriptions.register_topic(tp.first, p);
					std::cout << name << " adding topic " << tp.first << " via " << peer_name << std::endl;
					}

				sub_mapping[tp][p] = 42;
			}
		//peer_subscriptions.insert(subscriber{p, ts});
	 }

	void attach(std::string topic_or_id, caf::actor a)
		{
    std::cout << name  <<  " recvd local sub for topic "  
					<< topic_or_id  << " from " << this->id() << std::endl;
		demonitor(a);
		monitor(a);

		topic_actor_pair tp = std::make_pair(topic_or_id, this->address());
		sub_topic_actor.insert(tp);

		local_subscriptions.register_topic(topic_or_id, std::move(a));

		if ( (behavior_flags & AUTO_ADVERTISE) ||
		     advert_acls.find(topic_or_id) != advert_acls.end() )
			advertise_subscription(std::move(topic_or_id));
		}

	caf::actor find_master(const store::identifier& id)
		{
		auto m = local_subscriptions.exact_match(id);

		if ( ! m )
			m = peer_subscriptions.exact_match(id);

		if ( ! m )
			return caf::invalid_actor;

		return *m->begin();
		}

	void advertise_subscription(topic t)
		{
		advertise_subscription(t, this->address());
		}

	void advertise_subscription(topic t, caf::actor_addr a)
		{
		if ( advertised_subscriptions.insert({t, true}).second )
			{
			BROKER_DEBUG("endpoint." + name,"Advertise new subscription: " + t);
			publish_subscription_operation(std::move(t), sub_atom::value, a);
			}
		}

	void unadvertise_subscription(topic t)
		{
		unadvertise_subscription(t, this->address());
		}

	void unadvertise_subscription(topic t, caf::actor_addr a)
		{
		if ( advertised_subscriptions.erase(t) )
			{
			BROKER_DEBUG("endpoint." + name, "Unadvertise subscription: " + t);
			publish_subscription_operation(std::move(t), unsub_atom::value, a);
			}
		}

	void publish_subscription_operation(topic t, caf::atom_value op, caf::actor_addr sub_id)
		{
		publish_subscription_operation(t, caf::actor(), op, sub_id);
		}

	void publish_subscription_operation(topic t, const caf::actor& skip, caf::atom_value op, caf::actor_addr sub_id)
		{
		if ( peers.empty() )
			return;

    auto msg = caf::make_message(std::move(op), t, this, sub_id);
		for ( const auto& p : peers )
    	{
			if(p.second.ep == skip)
				continue;
     	std::cout << name  << ": " << caf::to_string(op) << " for topic (" 
								<< t << "," << caf::to_string(sub_id) << ")" << ", forward to peer " 
								<< p.second.name  << std::endl;
      send(p.second.ep, msg);
      }
		}

	void publish_locally(const topic& t, broker::message msg, int flags,
	                     bool from_peer)
		{
		if ( ! from_peer && ! (flags & SELF) )
			return;

		auto matches = local_subscriptions.prefix_matches(t);

		if ( matches.empty() )
			return;

		std::cout << name << " msg for topic " <<  t << " published locally" << std::endl;

		auto caf_msg = caf::make_message(std::move(msg));

		for ( const auto& match : matches )
			for ( const auto& a : match->second )
				send(a, caf_msg);
		}

	void publish_current_msg_to_peers(const topic& t, int flags)
		{
		if ( ! (flags & PEERS) )
			{
			return;
			}

		if ( ! (behavior_flags & AUTO_PUBLISH) &&
		     pub_acls.find(t) == pub_acls.end() ) {
			// Not allowed to publish this topic to peers.
			return;
		}

    // send instead of forward_to so peer can use
    // current_sender() to check if msg comes from a peer.
    if ( (flags & UNSOLICITED) )
        {
        for ( const auto& p : peers ) 
            {
            if(current_sender() == p.first)
                continue;
            send(p.second.ep, current_message());
            }
        }
    else
        {
				if(peer_subscriptions.unique_prefix_matches(t).empty())
					std::cout << name << " no routing information for forwarding topic " << t << std::endl;

        for ( const auto& a : peer_subscriptions.unique_prefix_matches(t) )
            {
						if(current_sender() == a)
							continue;
						std::cout  <<  name << ": --- send msg to " << get_peer_name(a) << std::endl;
            send(a, current_message());
            }
        }
    }

	//FIXME currently not the most efficient way 
	// radix_tree +operator required
	topic_set get_all_subscriptions()
		{
		topic_set subscr = advertised_subscriptions;
		for(auto& peer: peers)
			{
			topic_set ts2 = peer_subscriptions.topics_of_actor(peer.second.ep.address());
			for(auto& t: ts2)
				subscr.insert(t);
			}
		return subscr;
		}

	/**
	 * update the routing information for topics
	 * when actor p unpeers from us
	 */
	void update_routing_information(const caf::actor& p)
		{
		std::cout << name  << " update routing information after unpeering of peer " << get_peer_name(p) << std::endl;
		if(routing_info.find(p.address()) == routing_info.end())
			return;

		// update routing information for peer_subscriptions
		for(auto& tp: routing_info[p.address()])
			update_routing_information_topic(p, tp);

		routing_info.erase(p.address());
		}

	void update_routing_information_topic(const caf::actor& p, topic_actor_pair tp)
		{
		std::cout << "   - " << name << "  check entry for topic (" << tp.first << ", " << caf::to_string(tp.second) << ")" << std::endl;
		if(tp.second != this->address() && sub_mapping.find(tp) != sub_mapping.end() && sub_mapping[tp].find(p) != sub_mapping[tp].end())
			{
			std::cout << name  << ": delete peer " <<  get_peer_name(p) << " from sub_mapping" << std::endl;
			sub_mapping[tp].erase(p);
			if(sub_mapping[tp].empty())
				{
				std::cout << "  - " <<  name  << ": delete entry (" << tp.first << ", " << caf::to_string(tp.second) << ")" << std::endl;
				sub_mapping.erase(tp);
				sub_topic_actor.erase(tp);
				// There is no other peer left for this topic, thus unsubscribe!
				publish_subscription_operation(tp.first, p, unsub_atom::value, tp.second);
				}
			else
				{
				caf::actor a;
				int ttl = 424242;
				for(auto& p: sub_mapping[tp])
					{
					if(p.second < ttl)	
						{
						ttl = p.second;
						a = p.first;
						}
					}
				assert(a.id() != 0);
				// Another peer has registered for the same topic
				peer_subscriptions.register_topic(tp.first, a);
				std::cout << name << " found alternative source for topic " << tp.first << caf::to_string(a) << std::endl; 
				}
			} 
		}


	struct peer_endpoint {
		caf::actor ep;
		std::string name;
		bool incoming;
	};

	caf::behavior active;

	std::string name;
	int behavior_flags;
	topic_set pub_acls;
	topic_set advert_acls;

	std::unordered_map<caf::actor_addr, peer_endpoint> peers;
	subscription_registry local_subscriptions;
	subscription_registry peer_subscriptions;
	topic_set advertised_subscriptions;

	topic_actor_set sub_topic_actor;
	topic_actor_mapping sub_mapping;
	routing_information routing_info;
};

/**
 * Manages connection to a remote endpoint_actor including auto-reconnection
 * and associated peer/unpeer messages.
 */
class endpoint_proxy_actor : public caf::event_based_actor {

public:

	endpoint_proxy_actor(caf::actor local, std::string endpoint_name,
	                     std::string addr, uint16_t port,
	                     std::chrono::duration<double> retry_freq,
	                     caf::actor ocs_queue)
		{
		using namespace caf;
		using namespace std;
		peering::impl pi(local, this, true, make_pair(addr, port));

		trap_exit(true);

		bootstrap = {
		after(chrono::seconds(0)) >> [=]
			{
			try_connect(pi, endpoint_name);
			}
		};

		disconnected = {
		[=](peerstat_atom)
			{
			ocs_update(ocs_queue, pi,
		               outgoing_connection_status::tag::disconnected);
			},
		[=](const exit_msg& e)
			{
			quit();
			},
		[=](quit_atom)
			{
			quit();
			},
		after(chrono::duration_cast<chrono::microseconds>(retry_freq)) >> [=]
			{
			try_connect(pi, endpoint_name);
			}
		};

		connected = {
		[=](peerstat_atom)
			{
			send(local, peer_atom::value, remote, pi);
			},
		[=](const exit_msg& e)
			{
			send(remote, unpeer_atom::value, local);
			send(local, unpeer_atom::value, remote);
			quit();
			},
		[=](quit_atom)
			{
			send(remote, unpeer_atom::value, local);
			send(local, unpeer_atom::value, remote);
			quit();
			},
		[=](const down_msg& d)
			{
			BROKER_DEBUG(report_subtopic(endpoint_name, addr, port),
			             "Disconnected from peer");
			demonitor(remote);
			remote = invalid_actor;
			become(disconnected);
			ocs_update(ocs_queue, pi,
		               outgoing_connection_status::tag::disconnected);
			},
		others() >> [=]
			{
			report::warn(report_subtopic(endpoint_name, addr, port),
			             "Remote endpoint proxy got unexpected message: "
			             + caf::to_string(current_message()));
			}
		};
		}

private:

	caf::behavior make_behavior() override
		{
		return bootstrap;
		}

	std::string report_subtopic(const std::string& endpoint_name,
	                            const std::string& addr, uint16_t port) const
		{
		std::ostringstream st;
		st << "endpoint." << endpoint_name << ".remote_proxy." << addr
		   << ":" << port;
		return st.str();
		}

	bool try_connect(const peering::impl& pi, const std::string& endpoint_name)
		{
		using namespace caf;
		using namespace std;
		const std::string& addr = pi.remote_tuple.first;
		const uint16_t& port = pi.remote_tuple.second;
		const caf::actor& local = pi.endpoint_actor;

		try
			{
			remote = io::remote_actor(addr, port);
			}
		catch ( const exception& e )
			{
			report::warn(report_subtopic(endpoint_name, addr, port),
			             string("Failed to connect: ") + e.what());
			}

		if ( ! remote )
			{
			become(disconnected);
			return false;
			}

		BROKER_DEBUG(report_subtopic(endpoint_name, addr, port), "Connected");
		monitor(remote);
		become(connected);
		send(local, peer_atom::value, remote, pi);
		return true;
		}

	caf::actor remote = caf::invalid_actor;
	caf::behavior bootstrap;
	caf::behavior disconnected;
	caf::behavior connected;
};

static inline caf::actor& handle_to_actor(void* h)
	{ return *static_cast<caf::actor*>(h); }

class endpoint::impl {
public:

	impl(const endpoint* ep, std::string n, int arg_flags)
		: name(std::move(n)), flags(arg_flags), self(),
		  outgoing_conns(), incoming_conns(),
		  actor(caf::spawn<broker::endpoint_actor>(ep, name, flags,
		                   handle_to_actor(outgoing_conns.handle()),
		                   handle_to_actor(incoming_conns.handle()))),
		  peers(), last_errno(), last_error()
		{
		self->planned_exit_reason(caf::exit_reason::user_defined);
		actor->link_to(self);
		}

	std::string name;
	int flags;
	caf::scoped_actor self;
	outgoing_connection_status_queue outgoing_conns;
	incoming_connection_status_queue incoming_conns;
	caf::actor actor;
	std::unordered_set<peering> peers;
	int last_errno;
	std::string last_error;
};
} // namespace broker

#endif // BROKER_ENDPOINT_IMPL_HH
