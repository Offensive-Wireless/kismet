/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "config.h"

#include "packetchain.h"
#include "packet.h"
#include "trackedcomponent.h"
#include "eventbus.h"

// Common packet filter mechanism which can be used in multiple locations;
// implements basic default behavior, filtering by address, and REST endpoints.
//
// Filters act on 'true' results:  Default behavior of 'true' defaults to BLOCKING packets.
// Default behavior of 'false' defaults to PASSING packets.

class Packetfilter : public tracker_component {
public:
    Packetfilter(const std::string& in_id, const std::string& in_description,
            const std::string& in_type);

    virtual ~Packetfilter() {
        local_locker l(&mutex);
    }

    __ProxyGet(filter_id, std::string, std::string, filter_id);
    __ProxyGet(filter_description, std::string, std::string, filter_description);
    __ProxyGet(filter_type, std::string, std::string, filter_type);
    __Proxy(filter_default, uint8_t, bool, bool, filter_default);

    virtual bool filter_packet(kis_packet *packet) = 0;

protected:
    bool filterstring_to_bool(const std::string& str);

    __ProxySet(filter_id, std::string, std::string, filter_id);
    __ProxySet(filter_description, std::string, std::string, filter_description);
    __ProxySet(filter_type, std::string, std::string, filter_type);

    virtual void register_fields() override {
        tracker_component::register_fields();

        register_field("kismet.packetfilter.id", "Filter ID/Endpoint", &filter_id);
        register_field("kismet.packetfilter.description", "Filter description", &filter_description);
        register_field("kismet.packetfilter.type", "Filter mechanism", &filter_type);
        register_field("kismet.packetfilter.default", "Default filter (pass/reject)", &filter_default);
    }

    kis_recursive_timed_mutex mutex;

    std::string base_uri;

    std::shared_ptr<tracker_element_string> filter_id;
    std::shared_ptr<tracker_element_string> filter_description;
    std::shared_ptr<tracker_element_string> filter_type;
    std::shared_ptr<tracker_element_uint8> filter_default;

    // Default endpoint
    std::shared_ptr<kis_net_httpd_simple_post_endpoint> default_endp;
    int default_set_endp_handler(std::ostream& stream, shared_structured post_structured);

    // Default display endpoint
    std::shared_ptr<kis_net_httpd_simple_tracked_endpoint> self_endp;
    // Build the return object; subfilters must implement this to bypass class heirarchy & call
    // build_self_content
    virtual std::shared_ptr<tracker_element_map> self_endp_handler() = 0;
    // Cascading build
    virtual void build_self_content(std::shared_ptr<tracker_element_map> content);
};

// Mac-address based filter.
// Filters can be applied to 'ANY', source, destination, network, or transmitter
// (in wifi terms, source, dest, bssid, or 4mac transmitter for wds).
// Filters are true (filter/reject packet), or false (pass packet).  Packets not matched
// by any filter are passed to the default filter term.
class packet_filter_mac_addr : public Packetfilter {
public:
    packet_filter_mac_addr(const std::string& in_id, const std::string& in_description);
    virtual ~packet_filter_mac_addr();

    virtual bool filter_packet(kis_packet *packet) override;

    // We use strings for blocks here for maximum flexibility in the future since
    // *adding* a filter should be a relatively non-realtime task
    virtual void set_filter(mac_addr in_mac, const std::string& in_phy,
            const std::string& in_block, bool value);
    virtual void remove_filter(mac_addr in_mac, const std::string &in_phy,
            const std::string& in_block);

protected:
    virtual void register_fields() override {
        Packetfilter::register_fields();

		// Phy-based map
        register_field("kismet.packetfilter.macaddr.blocks_by_phy",
                "MAC address filters", &filter_phy_blocks);

        filter_sub_value_id =
            register_field("kismet.packetfilter.macaddr.value",
                    tracker_element_factory<tracker_element_uint8>(),
                    "Filter value");

        filter_source_id =
            register_field("kismet.packetfilter.macaddr.source", 
                    tracker_element_factory<tracker_element_mac_map>(),
                    "Source address filters");

        filter_dest_id =
            register_field("kismet.packetfilter.macaddr.destination", 
                    tracker_element_factory<tracker_element_mac_map>(),
                    "Destination address filters");

        filter_network_id =
            register_field("kismet.packetfilter.macaddr.network", 
                    tracker_element_factory<tracker_element_mac_map>(),
                    "Network/BSSID address filters");

        filter_other_id =
            register_field("kismet.packetfilter.macaddr.other", 
                    tracker_element_factory<tracker_element_mac_map>(),
                    "Other address filters");

        filter_any_id =
            register_field("kismet.packetfilter.macaddr.any", 
                    tracker_element_factory<tracker_element_mac_map>(),
                    "Any matching address type");
    }

    std::shared_ptr<device_tracker> devicetracker;
	std::shared_ptr<event_bus> eventbus;
	unsigned long eb_id;

	void update_phy_map(std::shared_ptr<eventbus_event> evt);

    unsigned int pack_comp_common;

    int filter_sub_value_id, filter_source_id, filter_dest_id, 
        filter_network_id, filter_other_id, filter_any_id;

    // Externally exposed tracked table
    std::shared_ptr<tracker_element_string_map> filter_phy_blocks;

    struct phy_filter_group {
        std::map<mac_addr, bool> filter_source;
        std::map<mac_addr, bool> filter_dest;
        std::map<mac_addr, bool> filter_network;
        std::map<mac_addr, bool> filter_other;
        std::map<mac_addr, bool> filter_any;
    };

	// Internal fast lookup tables per-phy we use for actual filtering
	std::map<int, struct phy_filter_group> phy_mac_filter_map;
	// Internal unknown phy map for filters registered before we had a phy ID
	std::map<std::string, struct phy_filter_group> unknown_phy_mac_filter_map;

    // Address management endpoint keyed on path
    std::shared_ptr<Kis_Net_Httpd_Path_Post_Endpoint> macaddr_edit_endp;
    unsigned int edit_endp_handler(std::ostream& stream, const std::vector<std::string>& path, 
            shared_structured structured);

    std::shared_ptr<Kis_Net_Httpd_Path_Post_Endpoint> macaddr_remove_endp;
    unsigned int remove_endp_handler(std::ostream& stream, const std::vector<std::string> &path,
            shared_structured structured);

    virtual std::shared_ptr<tracker_element_map> self_endp_handler() override;
    virtual void build_self_content(std::shared_ptr<tracker_element_map> content) override;
};

