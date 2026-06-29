#!/bin/bash
# backtest_market.sh — Systematic backtest of different market universes
# Usage: ./backtest_market.sh [segment] [range]

set -e

BACKTEST="./nox_backtest"
RANGE="${2:-2y}"

# Market segments
declare -A SEGMENTS=(
    ["mega"]="AAPL,MSFT,GOOGL,AMZN,NVDA,META,TSLA"
    ["tech"]="INTC,AMD,QCOM,MU,AMAT,LRCX,CDNS,SNPS,ASML,ARM"
    ["healthcare"]="JNJ,PFE,AMGN,LLY,ABBV,MRK,TMO,REGN,VRTX,BIIB"
    ["financials"]="JPM,BAC,WFC,GS,C,BLK,BX,KKR,SCHW,COIN"
    ["consumer"]="WMT,COST,MCD,NKE,SBUX,DIS,AMZN,BKNG,CCL,MAR"
    ["energy"]="XOM,CVX,COP,MPC,PSX,OKE,SLB,EOG,FANG,COG"
    ["industrials"]="BA,CAT,HON,RTX,GD,LMT,ETN,MMM,ITW,GE"
    ["chinese"]="BABA,JD,BILI,PDD,DIDI,NIO,XPeng,NTES,BIDU,IQ"
    ["mega_chinese"]="AAPL,MSFT,BABA,JD,NVDA,TSLA"
    ["all_tech_100"]="AAPL,MSFT,GOOGL,NVDA,META,TSLA,INTC,AMD,QCOM,MU,AMAT,ASML,ARM,LRCX,CDNS,SNPS,NVMI,ON,MXIM,ORCL,PYPL,SHOP,DDOG,SNOW,SPLK,NOW,ENPH,SMCI,MELI,UBER,COIN,SQ,DECK,NFLX,ROKU,DIS,AMZN,BKNG,COST,AXP,V,MA"
)

if [ -z "${SEGMENTS[$1]}" ] && [ "$1" != "all" ]; then
    echo "Usage: $0 [segment] [range]"
    echo ""
    echo "Available segments:"
    for seg in "${!SEGMENTS[@]}"; do
        echo "  $seg"
    done
    echo ""
    echo "Examples:"
    echo "  ./backtest_market.sh mega 2y"
    echo "  ./backtest_market.sh chinese 1y"
    echo "  ./backtest_market.sh all_tech_100 2y"
    echo "  ./backtest_market.sh all 2y  # Run all segments"
    exit 1
fi

run_segment() {
    local segment=$1
    local watchlist=${SEGMENTS[$segment]}
    echo ""
    echo "════════════════════════════════════════════════════════════"
    echo "📊 Testing: $segment (range: $RANGE)"
    echo "════════════════════════════════════════════════════════════"
    echo ""

    timeout 300 "$BACKTEST" watchlist="$watchlist" range="$RANGE" entry_slip=0.15 exit_slip=0.15 2>&1

    echo ""
    echo "✅ $segment complete"
}

if [ "$1" == "all" ]; then
    for segment in "${!SEGMENTS[@]}"; do
        run_segment "$segment"
    done
else
    run_segment "$1"
fi
