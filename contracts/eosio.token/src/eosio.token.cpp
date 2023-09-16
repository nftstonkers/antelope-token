#include <eosio.token/eosio.token.hpp>

namespace eosio {

void token::create( const name& issuer, const asset& maximum_supply ) {
    require_auth( get_self() );

    auto sym = maximum_supply.symbol;
    check( sym.is_valid(), "invalid symbol name" );
    check( maximum_supply.is_valid(), "invalid supply");
    check( maximum_supply.amount > 0, "max-supply must be positive");

    stats statstable( get_self(), sym.code().raw() );
    auto existing = statstable.find( sym.code().raw() );
    check( existing == statstable.end(), "token with symbol already exists" );

    statstable.emplace( get_self(), [&]( auto& s ) {
       s.supply.symbol = maximum_supply.symbol;
       s.max_supply    = maximum_supply;
       s.issuer        = issuer;
    });
}

void token::setfee( const name& issuer, const symbol& symbol, const uint8_t fees ) {
    require_auth(issuer);

    check( fees < 50, "Max fee allowed - 0.5%");
    check( symbol.is_valid(), "invalid symbol name" );

    stats statstable( get_self(), symbol.code().raw() );
    auto existing = statstable.find( symbol.code().raw() );
    check( existing != statstable.end(), "token doesn't exist" );
    check( existing->issuer == issuer, "issuer not authorized" );

    statstable.modify(existing, same_payer, [&]( auto& s ) {
       s.fees = fees;
    });
}


void token::issue( const name& to, const asset& quantity, const string& memo )
{
    auto sym = quantity.symbol;
    check( sym.is_valid(), "invalid symbol name" );
    check( memo.size() <= 256, "memo has more than 256 bytes" );

    stats statstable( get_self(), sym.code().raw() );
    auto existing = statstable.find( sym.code().raw() );
    check( existing != statstable.end(), "token with symbol does not exist, create token before issue" );
    const auto& st = *existing;
    check( to == st.issuer, "tokens can only be issued to issuer account" );

    require_auth( st.issuer );
    check( quantity.is_valid(), "invalid quantity" );
    check( quantity.amount > 0, "must issue positive quantity" );

    check( quantity.symbol == st.supply.symbol, "symbol precision mismatch" );
    check( quantity.amount <= st.max_supply.amount - st.supply.amount, "quantity exceeds available supply");

    statstable.modify( st, same_payer, [&]( auto& s ) {
       s.supply += quantity;
    });

    add_balance( st.issuer, quantity, st.issuer );
}

void token::retire( const asset& quantity, const string& memo )
{
    auto sym = quantity.symbol;
    check( sym.is_valid(), "invalid symbol name" );
    check( memo.size() <= 256, "memo has more than 256 bytes" );

    stats statstable( get_self(), sym.code().raw() );
    auto existing = statstable.find( sym.code().raw() );
    check( existing != statstable.end(), "token with symbol does not exist" );
    const auto& st = *existing;

    require_auth( st.issuer );
    check( quantity.is_valid(), "invalid quantity" );
    check( quantity.amount > 0, "must retire positive quantity" );

    check( quantity.symbol == st.supply.symbol, "symbol precision mismatch" );

    statstable.modify( st, same_payer, [&]( auto& s ) {
       s.supply -= quantity;
    });

    sub_balance( st.issuer, quantity );
}

void token::transfer( const name&    from,
                      const name&    to,
                      const asset&   quantity,
                      const string&  memo )
{
    check( from != to, "cannot transfer to self" );
    require_auth( from );
    check( is_account( to ), "to account does not exist");

   // Ensure symbol is valid
    auto sym = quantity.symbol.code();
    stats statstable( get_self(), sym.raw() );
    const auto& st = statstable.get( sym.raw(), "no balance with specified symbol" );

    require_recipient( from );
    require_recipient( to );

    check( quantity.is_valid(), "invalid quantity" );
    check( quantity.amount > 0, "must transfer positive quantity" );
    check( quantity.symbol == st.supply.symbol, "symbol precision mismatch" );
    check( memo.size() <= 256, "memo has more than 256 bytes" );

    auto payer = has_auth( to ) ? to : from;

    asset fee = compute_fee(quantity, st.fees);

    exemptions_table exempts(get_self(), quantity.symbol.code().raw());
    bool is_exempted = exempts.find(from.value) != exempts.end();


    if(is_exempted) {
      sub_balance( from, quantity );
      add_balance( to, quantity - fee, payer );
      logfee(to, fee);
    } else {
      sub_balance( from, quantity + fee );
      add_balance( to, quantity, payer );
      logfee(from, fee);
    }
    add_balance( st.issuer, fee, payer );
}

void token::logfee( const name& account, const asset& fee) {
   require_auth(get_self());
}

void token::sub_balance( const name& owner, const asset& value ) {
   accounts from_acnts( get_self(), owner.value );

   const auto& from = from_acnts.get( value.symbol.code().raw(), "no balance object found" );
   check( from.balance.amount >= value.amount, "overdrawn balance" );
   check( !from.is_frozen, "Sender account is frozen" );

   from_acnts.modify( from, owner, [&]( auto& a ) {
         a.balance -= value;
      });
}

void token::add_balance( const name& owner, const asset& value, const name& ram_payer )
{
   accounts to_acnts( get_self(), owner.value );
   auto to = to_acnts.find( value.symbol.code().raw() );

   if( to == to_acnts.end() ) {
      to_acnts.emplace( ram_payer, [&]( auto& a ){
        a.balance = value;
      });
   } else {
      check( !to->is_frozen, "Receiver account is frozen" );
      to_acnts.modify( to, same_payer, [&]( auto& a ) {
        a.balance += value;
      });
   }
}

void token::open( const name& owner, const symbol& symbol, const name& ram_payer )
{
   require_auth( ram_payer );

   check( is_account( owner ), "owner account does not exist" );

   auto sym_code_raw = symbol.code().raw();
   stats statstable( get_self(), sym_code_raw );
   const auto& st = statstable.get( sym_code_raw, "symbol does not exist" );
   check( st.supply.symbol == symbol, "symbol precision mismatch" );

   accounts acnts( get_self(), owner.value );
   auto it = acnts.find( sym_code_raw );
   if( it == acnts.end() ) {
      acnts.emplace( ram_payer, [&]( auto& a ){
        a.balance = asset{0, symbol};
      });
   }
}

void token::close( const name& owner, const symbol& symbol )
{
   require_auth( owner );
   accounts acnts( get_self(), owner.value );
   auto it = acnts.find( symbol.code().raw() );
   check( it != acnts.end(), "Balance row already deleted or never existed. Action won't have any effect." );
   check( it->balance.amount == 0, "Cannot close because the balance is not zero." );
   acnts.erase( it );
}

void token::freeze( const name& account, const symbol& symbol, const bool& status ) {
   stats statstable( get_self(), symbol.code().raw() );
   const auto& st = statstable.get( symbol.code().raw(), "Token with symbol does not exist" );
   
   // Only token issuer can freeze/unfreeze
   require_auth( st.issuer ); 

   accounts acnts( get_self(), account.value );
   const auto& acc = acnts.get( symbol.code().raw(), "Account not found" );
   acnts.modify( acc, same_payer, [&]( auto& a ) {
      a.is_frozen = status;
   });
}

asset token::compute_fee(const asset& quantity, uint8_t fee) {
    int64_t fee_amount = (quantity.amount / 10000 ) * fee; 
    return asset(fee_amount, quantity.symbol);
}

void token::switchexempt(const name& issuer, const symbol& symbol, const name& account) {
    // Authorization check
    require_auth(issuer);

    // Input validation
    check( symbol.is_valid(), "invalid symbol name" );
    check( is_account( account ), "invalid account" );

    // Verify if the token exists and if the issuer has authority over it
    stats statstable( get_self(), symbol.code().raw() );
    auto existing = statstable.require_find( symbol.code().raw(), "token with specified symbol doesn't exist" );
    check(existing->issuer == issuer, "issuer not authorized");

    // Switch the exemption status of the account
    exemptions_table exempts(get_self(), symbol.code().raw());
    auto itr = exempts.find(account.value);

    if (itr == exempts.end()) {
        // Add to exemptions if not already there
        exempts.emplace(get_self(), [&](auto& row){
            row.account = account;
        });
    } else {
        // Remove from exemptions if already there
        exempts.erase(itr);
    }
}


} /// namespace eosio
