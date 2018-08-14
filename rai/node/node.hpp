#pragma once

#include <rai/lib/work.hpp>
#include <rai/node/bootstrap.hpp>
#include <rai/node/stats.hpp>
#include <rai/node/wallet.hpp>
#include <rai/secure/ledger.hpp>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_set>

#include <boost/asio.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/log/trivial.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/multi_index_container.hpp>

#include <miniupnpc.h>

#include <ed25519-donna/ed25519-donna.h>

namespace boost
{
namespace program_options
{
	class options_description;
	class variables_map;
}
}

namespace rai
{
rai::endpoint map_endpoint_to_v6 (rai::endpoint const &);
class node;
class election_status
{
public:
	std::shared_ptr<rai::block> winner;
	rai::amount tally;
	bool stapled;
};
class vote_info
{
public:
	std::chrono::steady_clock::time_point time;
	uint64_t sequence;
	rai::block_hash hash;
};
class election_vote_result
{
public:
	election_vote_result ();
	election_vote_result (bool, bool);
	bool replay;
	bool processed;
};
class election : public std::enable_shared_from_this<rai::election>
{
	std::function<void(std::shared_ptr<rai::block>)> confirmation_action;
	void confirm_once (MDB_txn *);

public:
	election (rai::node &, std::shared_ptr<rai::block>, std::function<void(std::shared_ptr<rai::block>)> const &);
	rai::election_vote_result vote (rai::account, uint64_t, rai::block_hash);
	rai::tally_t tally (MDB_txn *);
	// Check if we have vote quorum
	bool have_quorum (rai::tally_t const &);
	// Change our winner to agree with the network
	void compute_rep_votes (MDB_txn *);
	// Confirm this block if quorum is met
	void confirm_if_quorum (MDB_txn *);
	void log_votes (rai::tally_t const &);
	bool publish (std::shared_ptr<rai::block> block_a);
	void abort ();
	rai::node & node;
	std::unordered_map<rai::account, rai::vote_info> last_votes;
	std::unordered_map<rai::block_hash, std::shared_ptr<rai::block>> blocks;
	rai::block_hash root;
	rai::election_status status;
	std::atomic<bool> confirmed;
	bool aborted;
	std::unordered_map<rai::block_hash, rai::uint128_t> last_tally;
};
class conflict_info
{
public:
	rai::block_hash root;
	std::shared_ptr<rai::election> election;
	// Number of announcements in a row for this fork
	unsigned announcements;
	std::pair<std::shared_ptr<rai::block>, std::shared_ptr<rai::block>> confirm_req_options;
};
// Core class for determining consensus
// Holds all active blocks i.e. recently added blocks that need confirmation
class active_transactions
{
public:
	active_transactions (rai::node &);
	// Start an election for a block
	// Call action with confirmed block, may be different than what we started with
	bool start (std::shared_ptr<rai::block>, std::function<void(std::shared_ptr<rai::block>)> const & = [](std::shared_ptr<rai::block>) {});
	// Also supply alternatives to block, to confirm_req reps with if the boolean argument is true
	// Should only be used for old elections
	// The first block should be the one in the ledger
	bool start (std::pair<std::shared_ptr<rai::block>, std::shared_ptr<rai::block>>, std::function<void(std::shared_ptr<rai::block>)> const & = [](std::shared_ptr<rai::block>) {});
	// If this returns true, the vote is a replay
	// If this returns false, the vote may or may not be a replay
	bool vote (std::shared_ptr<rai::vote>);
	// Is the root of this block in the roots container
	bool active (rai::block const &);
	std::deque<std::shared_ptr<rai::block>> list_blocks ();
	void erase (rai::block const &);
	void stop ();
	bool publish (std::shared_ptr<rai::block> block_a);
	boost::multi_index_container<
	rai::conflict_info,
	boost::multi_index::indexed_by<
	boost::multi_index::hashed_unique<boost::multi_index::member<rai::conflict_info, rai::block_hash, &rai::conflict_info::root>>>>
	roots;
	std::unordered_map<rai::block_hash, std::shared_ptr<rai::election>> successors;
	std::deque<rai::election_status> confirmed;
	rai::node & node;
	std::mutex mutex;
	// Maximum number of conflicts to vote on per interval, lowest root hash first
	static unsigned constexpr announcements_per_interval = 32;
	// Minimum number of block announcements
	static unsigned constexpr announcement_min = 2;
	// Threshold to start logging blocks haven't yet been confirmed
	static unsigned constexpr announcement_long = 20;
	static unsigned constexpr announce_interval_ms = (rai::rai_network == rai::rai_networks::rai_test_network) ? 10 : 16000;
	static size_t constexpr election_history_size = 2048;

private:
	void announce_loop ();
	void announce_votes ();
	std::condition_variable condition;
	bool started;
	bool stopped;
	std::thread thread;
};
class operation
{
public:
	bool operator> (rai::operation const &) const;
	std::chrono::steady_clock::time_point wakeup;
	std::function<void()> function;
};
class alarm
{
public:
	alarm (boost::asio::io_service &);
	~alarm ();
	void add (std::chrono::steady_clock::time_point const &, std::function<void()> const &);
	void run ();
	boost::asio::io_service & service;
	std::mutex mutex;
	std::condition_variable condition;
	std::priority_queue<operation, std::vector<operation>, std::greater<operation>> operations;
	std::thread thread;
};
class gap_information
{
public:
	std::chrono::steady_clock::time_point arrival;
	rai::block_hash hash;
	std::unordered_set<rai::account> voters;
};
class gap_cache
{
public:
	gap_cache (rai::node &);
	void add (MDB_txn *, std::shared_ptr<rai::block>);
	void vote (std::shared_ptr<rai::vote>);
	rai::uint128_t bootstrap_threshold (MDB_txn *);
	void purge_old ();
	boost::multi_index_container<
	rai::gap_information,
	boost::multi_index::indexed_by<
	boost::multi_index::ordered_non_unique<boost::multi_index::member<gap_information, std::chrono::steady_clock::time_point, &gap_information::arrival>>,
	boost::multi_index::hashed_unique<boost::multi_index::member<gap_information, rai::block_hash, &gap_information::hash>>>>
	blocks;
	size_t const max = 256;
	std::mutex mutex;
	rai::node & node;
};
class work_pool;
class peer_information
{
public:
	peer_information (rai::endpoint const &, unsigned, boost::optional<rai::account> = boost::none);
	peer_information (rai::endpoint const &, std::chrono::steady_clock::time_point const &, std::chrono::steady_clock::time_point const &);
	rai::endpoint endpoint;
	boost::asio::ip::address ip_address;
	std::chrono::steady_clock::time_point last_contact;
	std::chrono::steady_clock::time_point last_attempt;
	std::chrono::steady_clock::time_point last_bootstrap_attempt;
	std::chrono::steady_clock::time_point last_rep_request;
	std::chrono::steady_clock::time_point last_rep_response;
	rai::amount rep_weight;
	rai::account probable_rep_account;
	unsigned network_version;
	boost::optional<rai::account> node_id;
};
class peer_attempt
{
public:
	rai::endpoint endpoint;
	std::chrono::steady_clock::time_point last_attempt;
};
class syn_cookie_info
{
public:
	rai::uint256_union cookie;
	std::chrono::steady_clock::time_point created_at;
};
class peer_by_ip_addr
{
};
class peer_container
{
public:
	peer_container (rai::endpoint const &);
	// We were contacted by endpoint, update peers
	// Returns true if a Node ID handshake should begin
	bool contacted (rai::endpoint const &, unsigned);
	// Unassigned, reserved, self
	bool not_a_peer (rai::endpoint const &, bool);
	// Returns true if peer was already known
	bool known_peer (rai::endpoint const &);
	// Notify of peer we received from
	bool insert (rai::endpoint const &, unsigned, boost::optional<rai::account> = boost::none);
	std::unordered_set<rai::endpoint> random_set (size_t);
	void random_fill (std::array<rai::endpoint, 8> &);
	// Request a list of the top known representatives
	std::vector<peer_information> representatives (size_t);
	// List of all peers
	std::deque<rai::endpoint> list ();
	std::map<rai::endpoint, unsigned> list_version ();
	std::vector<peer_information> list_vector ();
	// A list of random peers sized for the configured rebroadcast fanout
	std::deque<rai::endpoint> list_fanout ();
	// Get the next peer for attempting bootstrap
	rai::endpoint bootstrap_peer ();
	// Purge any peer where last_contact < time_point and return what was left
	std::vector<rai::peer_information> purge_list (std::chrono::steady_clock::time_point const &);
	void purge_syn_cookies (std::chrono::steady_clock::time_point const &);
	std::vector<rai::endpoint> rep_crawl ();
	bool rep_response (rai::endpoint const &, rai::account const &, rai::amount const &);
	void rep_request (rai::endpoint const &);
	// Should we reach out to this endpoint with a keepalive message
	bool reachout (rai::endpoint const &);
	// Returns boost::none if the IP is rate capped on syn cookie requests,
	// or if the endpoint already has a syn cookie query
	boost::optional<rai::uint256_union> assign_syn_cookie (rai::endpoint const &);
	// Returns false if valid, true if invalid (true on error convention)
	// Also removes the syn cookie from the store if valid
	bool validate_syn_cookie (rai::endpoint const &, rai::account, rai::signature);
	boost::optional<rai::public_key> node_id (rai::endpoint const &);
	size_t size ();
	size_t size_sqrt ();
	rai::uint128_t total_weight ();
	rai::uint128_t online_weight_minimum;
	bool empty ();
	std::mutex mutex;
	rai::endpoint self;
	boost::multi_index_container<
	peer_information,
	boost::multi_index::indexed_by<
	boost::multi_index::hashed_unique<boost::multi_index::member<peer_information, rai::endpoint, &peer_information::endpoint>>,
	boost::multi_index::ordered_non_unique<boost::multi_index::member<peer_information, std::chrono::steady_clock::time_point, &peer_information::last_contact>>,
	boost::multi_index::ordered_non_unique<boost::multi_index::member<peer_information, std::chrono::steady_clock::time_point, &peer_information::last_attempt>, std::greater<std::chrono::steady_clock::time_point>>,
	boost::multi_index::random_access<>,
	boost::multi_index::ordered_non_unique<boost::multi_index::member<peer_information, std::chrono::steady_clock::time_point, &peer_information::last_bootstrap_attempt>>,
	boost::multi_index::ordered_non_unique<boost::multi_index::member<peer_information, std::chrono::steady_clock::time_point, &peer_information::last_rep_request>>,
	boost::multi_index::ordered_non_unique<boost::multi_index::member<peer_information, rai::amount, &peer_information::rep_weight>, std::greater<rai::amount>>,
	boost::multi_index::ordered_non_unique<boost::multi_index::tag<peer_by_ip_addr>, boost::multi_index::member<peer_information, boost::asio::ip::address, &peer_information::ip_address>>,
	boost::multi_index::hashed_non_unique<boost::multi_index::member<peer_information, rai::account, &peer_information::probable_rep_account>>>>
	peers;
	boost::multi_index_container<
	peer_attempt,
	boost::multi_index::indexed_by<
	boost::multi_index::hashed_unique<boost::multi_index::member<peer_attempt, rai::endpoint, &peer_attempt::endpoint>>,
	boost::multi_index::ordered_non_unique<boost::multi_index::member<peer_attempt, std::chrono::steady_clock::time_point, &peer_attempt::last_attempt>>>>
	attempts;
	std::mutex syn_cookie_mutex;
	std::unordered_map<rai::endpoint, syn_cookie_info> syn_cookies;
	std::unordered_map<boost::asio::ip::address, unsigned> syn_cookies_per_ip;
	// Number of peers that don't support node ID
	size_t legacy_peers;
	// Called when a new peer is observed
	std::function<void(rai::endpoint const &)> peer_observer;
	std::function<void()> disconnect_observer;
	// Number of peers to crawl for being a rep every period
	static size_t constexpr peers_per_crawl = 12;
	// Maximum number of peers per IP (includes legacy peers)
	static size_t constexpr max_peers_per_ip = 4;
	// Maximum number of legacy peers per IP
	static size_t constexpr max_legacy_peers_per_ip = 2;
	// Maximum number of peers that don't support node ID
	static size_t constexpr max_legacy_peers = 250;
};
class send_info
{
public:
	uint8_t const * data;
	size_t size;
	rai::endpoint endpoint;
	std::function<void(boost::system::error_code const &, size_t)> callback;
};
class mapping_protocol
{
public:
	char const * name;
	int remaining;
	boost::asio::ip::address_v4 external_address;
	uint16_t external_port;
};
// These APIs aren't easy to understand so comments are verbose
class port_mapping
{
public:
	port_mapping (rai::node &);
	void start ();
	void stop ();
	void refresh_devices ();
	// Refresh when the lease ends
	void refresh_mapping ();
	// Refresh occasionally in case router loses mapping
	void check_mapping_loop ();
	int check_mapping ();
	bool has_address ();
	std::mutex mutex;
	rai::node & node;
	UPNPDev * devices; // List of all UPnP devices
	UPNPUrls urls; // Something for UPnP
	IGDdatas data; // Some other UPnP thing
	// Primes so they infrequently happen at the same time
	static int constexpr mapping_timeout = rai::rai_network == rai::rai_networks::rai_test_network ? 53 : 3593;
	static int constexpr check_timeout = rai::rai_network == rai::rai_networks::rai_test_network ? 17 : 53;
	boost::asio::ip::address_v4 address;
	std::array<mapping_protocol, 2> protocols;
	uint64_t check_count;
	bool on;
};
class block_arrival_info
{
public:
	std::chrono::steady_clock::time_point arrival;
	rai::block_hash hash;
	boost::optional<std::pair<rai::uint256_union, rai::signature>> vote_staple;
	bool confirmed;
	rai::amount staple_tally;
};
class rebroadcast_info
{
public:
	bool recent;
	boost::optional<std::pair<rai::uint256_union, rai::signature>> vote_staple;
	bool confirmed;
	rai::amount staple_tally;
};
// This class tracks blocks that are probably live because they arrived in a UDP packet
// This gives a fairly reliable way to differentiate between blocks being inserted via bootstrap or new, live blocks.
class block_arrival
{
public:
	// Return `true' to indicated an error if the block has already been inserted
	bool add (rai::block_hash const &, boost::optional<std::pair<rai::uint256_union, rai::signature>> = boost::none, bool = false, rai::amount = rai::amount (0));
	bool recent (rai::block_hash const &);
	rai::rebroadcast_info rebroadcast_info (rai::block_hash const &);
	boost::multi_index_container<
	rai::block_arrival_info,
	boost::multi_index::indexed_by<
	boost::multi_index::ordered_non_unique<boost::multi_index::member<rai::block_arrival_info, std::chrono::steady_clock::time_point, &rai::block_arrival_info::arrival>>,
	boost::multi_index::hashed_unique<boost::multi_index::member<rai::block_arrival_info, rai::block_hash, &rai::block_arrival_info::hash>>>>
	arrival;
	std::mutex mutex;
	static size_t constexpr arrival_size_min = 8 * 1024;
	static std::chrono::seconds constexpr arrival_time_min = std::chrono::seconds (300);
};
class rep_last_heard_info
{
public:
	std::chrono::steady_clock::time_point last_heard;
	rai::account representative;
};
class online_reps
{
public:
	online_reps (rai::node &);
	void vote (std::shared_ptr<rai::vote> const &);
	void recalculate_stake ();
	rai::uint128_t online_stake ();
	std::deque<rai::account> list ();
	boost::multi_index_container<
	rai::rep_last_heard_info,
	boost::multi_index::indexed_by<
	boost::multi_index::ordered_non_unique<boost::multi_index::member<rai::rep_last_heard_info, std::chrono::steady_clock::time_point, &rai::rep_last_heard_info::last_heard>>,
	boost::multi_index::hashed_unique<boost::multi_index::member<rai::rep_last_heard_info, rai::account, &rai::rep_last_heard_info::representative>>>>
	reps;
	rai::uint128_t online_stake_total;
	std::mutex mutex;
	rai::node & node;
};
class network
{
public:
	network (rai::node &, uint16_t);
	void receive ();
	void stop ();
	void receive_action (boost::system::error_code const &, size_t);
	void rpc_action (boost::system::error_code const &, size_t);
	void republish_vote (std::shared_ptr<rai::vote>);
	void republish_block (MDB_txn *, std::shared_ptr<rai::block>, bool = true);
	void republish (rai::block_hash const &, std::shared_ptr<std::vector<uint8_t>>, rai::endpoint);
	void publish_broadcast (std::vector<rai::peer_information> &, std::unique_ptr<rai::block>);
	void confirm_send (rai::confirm_ack const &, std::shared_ptr<std::vector<uint8_t>>, rai::endpoint const &);
	void merge_peers (std::array<rai::endpoint, 8> const &);
	void send_keepalive (rai::endpoint const &);
	void send_node_id_handshake (rai::endpoint const &, boost::optional<rai::uint256_union> const & query, boost::optional<rai::uint256_union> const & respond_to);
	void send_musig_stage0_req (rai::endpoint const &, std::shared_ptr<rai::state_block>, rai::account);
	void send_musig_stage0_res (rai::endpoint const &, rai::uint256_union, rai::uint256_union, rai::keypair);
	void send_musig_stage1_req (rai::endpoint const &, rai::uint256_union, rai::account, rai::public_key, rai::uint256_union);
	void send_musig_stage1_res (rai::endpoint const &, rai::uint256_union);
	void send_publish_vote_staple (rai::endpoint const &, std::shared_ptr<rai::state_block>, rai::uint256_union, rai::signature);
	void send_publish_vote_staple (std::shared_ptr<rai::state_block>, rai::uint256_union, rai::signature);
	void broadcast_confirm_req (std::shared_ptr<rai::block>);
	void broadcast_confirm_req_base (std::shared_ptr<rai::block>, std::shared_ptr<std::vector<rai::peer_information>>, unsigned);
	void send_confirm_req (rai::endpoint const &, std::shared_ptr<rai::block>);
	void send_buffer (uint8_t const *, size_t, rai::endpoint const &, std::function<void(boost::system::error_code const &, size_t)>);
	rai::endpoint endpoint ();
	rai::endpoint remote;
	std::array<uint8_t, 512> buffer;
	boost::asio::ip::udp::socket socket;
	std::mutex socket_mutex;
	boost::asio::ip::udp::resolver resolver;
	rai::node & node;
	bool on;
	static uint16_t const node_port = rai::rai_network == rai::rai_networks::rai_live_network ? 7075 : 54000;
};
class logging
{
public:
	logging ();
	void serialize_json (boost::property_tree::ptree &) const;
	bool deserialize_json (bool &, boost::property_tree::ptree &);
	bool upgrade_json (unsigned, boost::property_tree::ptree &);
	bool ledger_logging () const;
	bool ledger_duplicate_logging () const;
	bool vote_logging () const;
	bool network_logging () const;
	bool network_message_logging () const;
	bool network_publish_logging () const;
	bool network_packet_logging () const;
	bool network_keepalive_logging () const;
	bool network_node_id_handshake_logging () const;
	bool network_musig_logging () const;
	bool node_lifetime_tracing () const;
	bool insufficient_work_logging () const;
	bool log_rpc () const;
	bool bulk_pull_logging () const;
	bool callback_logging () const;
	bool work_generation_time () const;
	bool log_to_cerr () const;
	void init (boost::filesystem::path const &);

	bool ledger_logging_value;
	bool ledger_duplicate_logging_value;
	bool vote_logging_value;
	bool network_logging_value;
	bool network_message_logging_value;
	bool network_publish_logging_value;
	bool network_packet_logging_value;
	bool network_keepalive_logging_value;
	bool network_node_id_handshake_logging_value;
	bool network_musig_logging_value;
	bool node_lifetime_tracing_value;
	bool insufficient_work_logging_value;
	bool log_rpc_value;
	bool bulk_pull_logging_value;
	bool work_generation_time_value;
	bool log_to_cerr_value;
	bool flush;
	uintmax_t max_size;
	uintmax_t rotation_size;
	boost::log::sources::logger_mt log;
};
class node_init
{
public:
	node_init ();
	bool error ();
	bool block_store_init;
	bool wallet_init;
};
class node_config
{
public:
	node_config ();
	node_config (uint16_t, rai::logging const &);
	void serialize_json (boost::property_tree::ptree &) const;
	bool deserialize_json (bool &, boost::property_tree::ptree &);
	bool upgrade_json (unsigned, boost::property_tree::ptree &);
	rai::account random_representative ();
	uint16_t peering_port;
	rai::logging logging;
	std::vector<std::pair<std::string, uint16_t>> work_peers;
	std::vector<std::string> preconfigured_peers;
	std::vector<rai::account> preconfigured_representatives;
	unsigned bootstrap_fraction_numerator;
	rai::amount receive_minimum;
	rai::amount online_weight_minimum;
	unsigned online_weight_quorum;
	unsigned password_fanout;
	unsigned io_threads;
	unsigned work_threads;
	bool enable_voting;
	unsigned bootstrap_connections;
	unsigned bootstrap_connections_max;
	std::string callback_address;
	uint16_t callback_port;
	std::string callback_target;
	int lmdb_max_dbs;
	rai::stat_config stat_config;
	rai::uint256_union epoch_block_link;
	rai::account epoch_block_signer;
	std::chrono::system_clock::time_point generate_hash_votes_at;
	static std::chrono::seconds constexpr keepalive_period = std::chrono::seconds (60);
	static std::chrono::seconds constexpr keepalive_cutoff = keepalive_period * 5;
	static std::chrono::minutes constexpr wallet_backup_interval = std::chrono::minutes (5);
};
class node_observers
{
public:
	rai::observer_set<std::shared_ptr<rai::block>, rai::account const &, rai::uint128_t const &, bool> blocks;
	rai::observer_set<bool> wallet;
	rai::observer_set<std::shared_ptr<rai::vote>, rai::endpoint const &> vote;
	rai::observer_set<rai::account const &, bool> account_balance;
	rai::observer_set<rai::endpoint const &> endpoint;
	rai::observer_set<> disconnect;
	rai::observer_set<> started;
};
class vote_processor
{
public:
	vote_processor (rai::node &);
	void vote (std::shared_ptr<rai::vote>, rai::endpoint);
	rai::vote_code vote_blocking (MDB_txn *, std::shared_ptr<rai::vote>, rai::endpoint);
	void flush ();
	rai::node & node;
	void stop ();

private:
	void process_loop ();
	std::deque<std::pair<std::shared_ptr<rai::vote>, rai::endpoint>> votes;
	std::condition_variable condition;
	std::mutex mutex;
	bool started;
	bool stopped;
	bool active;
	std::thread thread;
};
// The network is crawled for representatives by occasionally sending a unicast confirm_req for a specific block and watching to see if it's acknowledged with a vote.
class rep_crawler
{
public:
	void add (rai::block_hash const &);
	void remove (rai::block_hash const &);
	bool exists (rai::block_hash const &);
	std::mutex mutex;
	std::unordered_set<rai::block_hash> active;
};
// Processing blocks is a potentially long IO operation
// This class isolates block insertion from other operations like servicing network operations
class block_processor
{
public:
	block_processor (rai::node &);
	~block_processor ();
	void stop ();
	void flush ();
	bool full ();
	void add (std::shared_ptr<rai::block>, std::chrono::steady_clock::time_point);
	void force (std::shared_ptr<rai::block>);
	bool should_log ();
	bool have_blocks ();
	void process_blocks ();
	rai::process_return process_receive_one (MDB_txn *, std::shared_ptr<rai::block>, std::chrono::steady_clock::time_point = std::chrono::steady_clock::now ());

private:
	void queue_unchecked (MDB_txn *, rai::block_hash const &);
	void process_receive_many (std::unique_lock<std::mutex> &);
	bool stopped;
	bool active;
	std::chrono::steady_clock::time_point next_log;
	std::deque<std::pair<std::shared_ptr<rai::block>, std::chrono::steady_clock::time_point>> blocks;
	std::unordered_set<rai::block_hash> blocks_hashes;
	std::deque<std::shared_ptr<rai::block>> forced;
	std::condition_variable condition;
	rai::node & node;
	std::mutex mutex;
};
class musig_stage0_info
{
public:
	musig_stage0_info (std::pair<rai::public_key, rai::uint256_union>, rai::account, std::shared_ptr<rai::state_block>, rai::uint256_union);
	std::chrono::steady_clock::time_point created;
	// pair of opposing node id, request id
	std::pair<rai::public_key, rai::uint256_union> session_id;
	rai::account representative;
	rai::uint256_union root;
	std::shared_ptr<rai::state_block> block;
	rai::uint256_union r_value;
};
class stapled_vote_info
{
public:
	stapled_vote_info (std::shared_ptr<rai::state_block>);
	std::chrono::steady_clock::time_point created;
	rai::uint256_union root;
	std::shared_ptr<rai::state_block> successor;
	rai::block_hash successor_hash;
};
class stapler_s_value_cache_key
{
public:
	rai::public_key node_id;
	rai::uint256_union request_id;
	rai::uint256_union rb_total;
};
bool operator== (rai::stapler_s_value_cache_key const &, rai::stapler_s_value_cache_key const &);
size_t hash_value (rai::stapler_s_value_cache_key const &);
class stapler_s_value_cache_value
{
public:
	rai::stapler_s_value_cache_key key;
	std::chrono::steady_clock::time_point created;
	rai::uint256_union l_base;
	rai::uint256_union agg_pubkey;
	rai::uint256_union s_value;
};
class vote_stapler
{
public:
	vote_stapler (rai::node &);
	rai::uint256_union stage0 (rai::transaction &, rai::public_key, rai::account, rai::uint256_union, std::shared_ptr<rai::state_block>);
	rai::uint256_union stage1 (rai::public_key, rai::uint256_union, rai::public_key, rai::uint256_union, rai::uint256_union);
	std::shared_ptr<rai::block> remove_root (rai::uint256_union);
	std::mutex mutex;
	boost::multi_index_container<
	rai::stapled_vote_info,
	boost::multi_index::indexed_by<
	boost::multi_index::hashed_unique<boost::multi_index::member<rai::stapled_vote_info, rai::uint256_union, &rai::stapled_vote_info::root>>,
	boost::multi_index::hashed_unique<boost::multi_index::member<rai::stapled_vote_info, rai::block_hash, &rai::stapled_vote_info::successor_hash>>>>
	stapled_votes;
	boost::multi_index_container<
	rai::musig_stage0_info,
	boost::multi_index::indexed_by<
	boost::multi_index::hashed_unique<boost::multi_index::member<rai::musig_stage0_info, std::pair<rai::public_key, rai::uint256_union>, &rai::musig_stage0_info::session_id>>,
	boost::multi_index::hashed_unique<boost::multi_index::member<rai::musig_stage0_info, rai::block_hash, &rai::musig_stage0_info::root>>>>
	stage0_info;
	boost::multi_index_container<
	rai::stapler_s_value_cache_value,
	boost::multi_index::indexed_by<
	boost::multi_index::hashed_unique<boost::multi_index::member<rai::stapler_s_value_cache_value, rai::stapler_s_value_cache_key, &rai::stapler_s_value_cache_value::key>>,
	boost::multi_index::ordered_non_unique<boost::multi_index::member<rai::stapler_s_value_cache_value, std::chrono::steady_clock::time_point, &rai::stapler_s_value_cache_value::created>>>>
	s_value_cache;
	rai::node & node;
};
class musig_request_info
{
public:
	musig_request_info (std::shared_ptr<rai::state_block>, std::function<void(bool, rai::uint256_union, rai::signature)> &&);
	std::shared_ptr<rai::state_block> block;
	rai::uint256_union block_hash;
	std::unordered_set<rai::account> reps_requested;
	std::function<void(bool, rai::uint256_union, rai::signature)> callback;
	std::chrono::steady_clock::time_point created;
};
class musig_stage0_status
{
public:
	musig_stage0_status (std::unordered_map<rai::account, std::vector<rai::endpoint>>);
	std::map<rai::account, rai::uint256_union> rb_values;
	rai::uint128_t vote_weight_collected;
	std::unordered_map<rai::account, std::vector<rai::endpoint>> rep_endpoints;
};
class vote_staple_requester
{
public:
	vote_staple_requester (rai::node &);
	void request_staple (std::shared_ptr<rai::state_block>, std::function<void(bool, rai::uint256_union, rai::signature)>);
	void request_staple_inner (std::shared_ptr<rai::state_block>, std::function<void(bool, rai::uint256_union, rai::signature)>);
	void musig_stage0_res (rai::endpoint const &, rai::musig_stage0_res const &);
	void musig_stage1_res (rai::musig_stage1_res const &);
	void calculate_weight_cutoff ();
	// Maps request IDs to block hashes
	std::unordered_map<rai::uint256_union, rai::block_hash> request_ids;
	std::unordered_map<rai::uint256_union, rai::musig_request_info> block_request_info;
	std::unordered_map<rai::block_hash, rai::musig_stage0_status> stage0_statuses;
	std::unordered_map<rai::uint256_union, rai::block_hash> stage1_sb_needed;
	std::unordered_map<rai::block_hash, rai::uint256_union> stage0_rb_totals;
	// Maps block hashes to a pair of the number of remaining s elements and the running total
	std::unordered_map<rai::block_hash, std::pair<size_t, std::array<bignum256modm_element_t, bignum256modm_limb_size>>> stage1_running_s_total;
	std::unordered_set<rai::account> blacklisted_reps;
	std::unordered_set<rai::block_hash> full_broadcast_blocks;
	rai::uint128_t weight_cutoff;
	std::unordered_map<rai::account, std::queue<std::pair<std::shared_ptr<rai::state_block>, std::function<void (bool, rai::uint256_union, rai::signature)>>>> accounts_queue;
	std::mutex mutex;
	bool force_full_broadcast;
	rai::node & node;
};
class rep_xor_solver
{
public:
	rep_xor_solver (rai::node &);
	void calculate_top_reps ();
	std::vector<std::vector<uint64_t *>> solve_xor_check (std::vector<uint64_t *>, uint64_t *, size_t, size_t);
	// Returns (total_stake, max_position). max_position is how far down the least important rep is in the list of top reps
	std::pair<rai::uint128_t, size_t> validate_staple (rai::block_hash block_hash, rai::uint256_union reps_xor, rai::signature signature);
	std::vector<rai::account> top_reps;
	std::vector<uint64_t *> top_rep_pointers;
	std::chrono::steady_clock::time_point last_calculated_top_reps;
	std::mutex mutex;
	rai::node & node;
};
class node : public std::enable_shared_from_this<rai::node>
{
public:
	node (rai::node_init &, boost::asio::io_service &, uint16_t, boost::filesystem::path const &, rai::alarm &, rai::logging const &, rai::work_pool &);
	node (rai::node_init &, boost::asio::io_service &, boost::filesystem::path const &, rai::alarm &, rai::node_config const &, rai::work_pool &);
	~node ();
	template <typename T>
	void background (T action_a)
	{
		alarm.service.post (action_a);
	}
	void send_keepalive (rai::endpoint const &);
	bool copy_with_compaction (boost::filesystem::path const &);
	void keepalive (std::string const &, uint16_t);
	void start ();
	void stop ();
	std::shared_ptr<rai::node> shared ();
	int store_version ();
	void process_confirmed (std::shared_ptr<rai::block>);
	void process_message (rai::message &, rai::endpoint const &);
	void process_active (std::shared_ptr<rai::block>);
	rai::process_return process (rai::block const &);
	void keepalive_preconfigured (std::vector<std::string> const &);
	rai::block_hash latest (rai::account const &);
	rai::uint128_t balance (rai::account const &);
	std::unique_ptr<rai::block> block (rai::block_hash const &);
	std::pair<rai::uint128_t, rai::uint128_t> balance_pending (rai::account const &);
	rai::uint128_t weight (rai::account const &);
	rai::account representative (rai::account const &);
	void ongoing_keepalive ();
	void ongoing_syn_cookie_cleanup ();
	void ongoing_rep_crawl ();
	void ongoing_bootstrap ();
	void ongoing_store_flush ();
	void backup_wallet ();
	int price (rai::uint128_t const &, int);
	void work_generate_blocking (rai::block &);
	uint64_t work_generate_blocking (rai::uint256_union const &);
	void work_generate (rai::uint256_union const &, std::function<void(uint64_t)>);
	void add_initial_peers ();
	void block_confirm (std::shared_ptr<rai::block>);
	void process_fork (MDB_txn *, std::shared_ptr<rai::block>);
	rai::uint128_t delta ();
	void vote_staple_broadcast (std::shared_ptr<rai::state_block>, std::function<void(bool)> = [](bool) {});
	void broadcast_block (std::shared_ptr<rai::block>);
	boost::asio::io_service & service;
	rai::node_config config;
	rai::alarm & alarm;
	rai::work_pool & work;
	boost::log::sources::logger_mt log;
	rai::block_store store;
	rai::gap_cache gap_cache;
	rai::ledger ledger;
	rai::active_transactions active;
	rai::network network;
	rai::bootstrap_initiator bootstrap_initiator;
	rai::bootstrap_listener bootstrap;
	rai::peer_container peers;
	boost::filesystem::path application_path;
	rai::node_observers observers;
	rai::wallets wallets;
	rai::port_mapping port_mapping;
	rai::vote_processor vote_processor;
	rai::rep_crawler rep_crawler;
	unsigned warmed_up;
	rai::block_processor block_processor;
	std::thread block_processor_thread;
	rai::block_arrival block_arrival;
	rai::online_reps online_reps;
	rai::stat stats;
	rai::keypair node_id;
	rai::vote_stapler vote_stapler;
	rai::vote_staple_requester vote_staple_requester;
	rai::rep_xor_solver rep_xor_solver;
	static double constexpr price_max = 16.0;
	static double constexpr free_cutoff = 1024.0;
	static std::chrono::seconds constexpr period = std::chrono::seconds (60);
	static std::chrono::seconds constexpr cutoff = period * 5;
	static std::chrono::seconds constexpr syn_cookie_cutoff = std::chrono::seconds (5);
	static std::chrono::minutes constexpr backup_interval = std::chrono::minutes (5);
	static size_t constexpr top_reps_hard_cutoff = 127;
	static size_t constexpr top_reps_confirmation_cutoff = 90;
	static size_t constexpr top_reps_generation_cutoff = 64;
	static size_t constexpr xor_check_possibilities_cap_log2 = 3;
};
class thread_runner
{
public:
	thread_runner (boost::asio::io_service &, unsigned);
	~thread_runner ();
	void join ();
	std::vector<std::thread> threads;
};
class inactive_node
{
public:
	inactive_node (boost::filesystem::path const & path = rai::working_path ());
	~inactive_node ();
	boost::filesystem::path path;
	boost::shared_ptr<boost::asio::io_service> service;
	rai::alarm alarm;
	rai::logging logging;
	rai::node_init init;
	rai::work_pool work;
	std::shared_ptr<rai::node> node;
};
}
